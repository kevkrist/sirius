// SPDX-License-Identifier: Apache-2.0
//
// C++-native .hpln v10 write/read for compressed_table.
// See compressed_table_io.hpp for the on-disk layout.

#include "api/compressed_table_io.hpp"

#include "codegen/plan/operator_registry.hpp"
#include "codegen/plan/plan_interpreter.hpp"
#include "codegen/plan/representation.hpp"

#include <cudf/column/column.hpp>
#include <cudf/column/column_factories.hpp>
#include <cudf/types.hpp>

#include <rmm/device_buffer.hpp>
#include <rmm/mr/per_device_resource.hpp>

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace simpatico {
namespace {

// ---------------------------------------------------------------------------
// LE binary helpers — std::bit_cast (C++20); asserts little-endian platform.
// ---------------------------------------------------------------------------
static_assert(std::endian::native == std::endian::little, "LE-only serialization");

[[nodiscard]] static bool is_valid_type_tag(std::uint8_t tag) noexcept
{
  for (auto const& [type, candidate] : kTypeTags) {
    (void)type;
    if (candidate == tag) return true;
  }
  return false;
}

[[nodiscard]] static bool is_serialized_leaf_kind(OpId kind) noexcept
{
  auto const tag = static_cast<std::uint8_t>(kind);
  return tag >= static_cast<std::uint8_t>(OpId::Delta) &&
         tag <= static_cast<std::uint8_t>(OpId::Zigzag);
}

[[nodiscard]] static bool fits_cudf_size_type(std::uint64_t rows) noexcept
{
  return rows <= static_cast<std::uint64_t>(std::numeric_limits<cudf::size_type>::max());
}

template <typename T>
static void push_le(std::vector<std::uint8_t>& v, T x)
{
  auto b = std::bit_cast<std::array<std::uint8_t, sizeof(T)>>(x);
  v.insert(v.end(), b.begin(), b.end());
}

static void push_str16(std::vector<std::uint8_t>& v, std::string const& s)
{
  push_le(v, static_cast<std::uint16_t>(s.size()));
  v.insert(v.end(), s.begin(), s.end());
}

struct Reader {
  const std::uint8_t* p = nullptr;
  std::size_t rem       = 0;

  bool read(void* dst, std::size_t n)
  {
    if (n > rem) return false;
    std::memcpy(dst, p, n);
    p += n;
    rem -= n;
    return true;
  }

  template <typename T>
  bool read_le(T& x)
  {
    std::array<std::uint8_t, sizeof(T)> b;
    if (!read(b.data(), sizeof(T))) return false;
    x = std::bit_cast<T>(b);
    return true;
  }

  bool read_str16(std::string& s)
  {
    std::uint16_t len;
    if (!read_le(len)) return false;
    if (len > rem) return false;
    s.assign(reinterpret_cast<const char*>(p), len);
    p += len;
    rem -= len;
    return true;
  }
};

// ---------------------------------------------------------------------------
// leaf_meta_v ↔ binary encoding
// meta_kind tags: 0=none 1=alp_rd 2=ans 3=bitcomp 4=cascaded 5=snappy 6=lz4 7=deflate
// ---------------------------------------------------------------------------
enum : std::uint8_t {
  META_NONE     = 0,
  META_ALP_RD   = 1,
  META_ANS      = 2,
  META_BITCOMP  = 3,
  META_CASCADED = 4,
  META_SNAPPY   = 5,
  META_LZ4      = 6,
  META_DEFLATE  = 7,
};

static void push_meta(std::vector<std::uint8_t>& v, leaf_meta_v const& m)
{
  struct Visitor {
    std::vector<std::uint8_t>& v;
    void operator()(leaf_meta::none const&) { push_le(v, META_NONE); }
    void operator()(leaf_meta::alp_rd const& a)
    {
      push_le(v, META_ALP_RD);
      push_le(v, a.right_bw);
    }
    void operator()(leaf_meta::ans const& a)
    {
      push_le(v, META_ANS);
      push_le(v, a.uncompressed_size);
      push_le(v, a.original_type_id);
    }
    void operator()(leaf_meta::bitcomp const& b)
    {
      push_le(v, META_BITCOMP);
      push_le(v, b.uncompressed_size);
      push_le(v, b.original_type_id);
      push_le(v, b.algorithm);
    }
    void operator()(leaf_meta::nvcomp_cascaded const& c)
    {
      push_le(v, META_CASCADED);
      push_le(v, c.uncompressed_size);
      push_le(v, c.original_type_id);
      push_le(v, c.num_deltas);
      push_le(v, c.num_RLEs);
      push_le(v, c.use_bp);
    }
    void operator()(leaf_meta::snappy const& s)
    {
      push_le(v, META_SNAPPY);
      push_le(v, s.uncompressed_size);
      push_le(v, s.original_type_id);
    }
    void operator()(leaf_meta::lz4 const& l)
    {
      push_le(v, META_LZ4);
      push_le(v, l.uncompressed_size);
      push_le(v, l.original_type_id);
    }
    void operator()(leaf_meta::deflate const& d)
    {
      push_le(v, META_DEFLATE);
      push_le(v, d.uncompressed_size);
      push_le(v, d.original_type_id);
    }
  };
  std::visit(Visitor{v}, m);
}

static bool read_meta(Reader& r, leaf_meta_v& out)
{
  std::uint8_t mk;
  if (!r.read_le(mk)) return false;
  switch (mk) {
    case META_NONE: out = leaf_meta::none{}; return true;
    case META_ALP_RD: {
      std::uint8_t rbw;
      if (!r.read_le(rbw)) return false;
      out = leaf_meta::alp_rd{rbw};
      return true;
    }
    case META_ANS: {
      leaf_meta::ans a;
      if (!r.read_le(a.uncompressed_size) || !r.read_le(a.original_type_id)) return false;
      out = a;
      return true;
    }
    case META_BITCOMP: {
      leaf_meta::bitcomp b;
      if (!r.read_le(b.uncompressed_size) || !r.read_le(b.original_type_id) ||
          !r.read_le(b.algorithm))
        return false;
      out = b;
      return true;
    }
    case META_CASCADED: {
      std::uint64_t us;
      std::int32_t ti, nd, nr, bp;
      if (!r.read_le(us) || !r.read_le(ti) || !r.read_le(nd) || !r.read_le(nr) || !r.read_le(bp))
        return false;
      out = leaf_meta::nvcomp_cascaded{us, ti, nd, nr, bp};
      return true;
    }
    case META_SNAPPY: {
      std::uint64_t us;
      std::int32_t ti;
      if (!r.read_le(us) || !r.read_le(ti)) return false;
      out = leaf_meta::snappy{us, ti};
      return true;
    }
    case META_LZ4: {
      std::uint64_t us;
      std::int32_t ti;
      if (!r.read_le(us) || !r.read_le(ti)) return false;
      out = leaf_meta::lz4{us, ti};
      return true;
    }
    case META_DEFLATE: {
      std::uint64_t us;
      std::int32_t ti;
      if (!r.read_le(us) || !r.read_le(ti)) return false;
      out = leaf_meta::deflate{us, ti};
      return true;
    }
    default: return false;
  }
}

// ---------------------------------------------------------------------------
// rep_from_leaf_desc: per-kind rep factory
// ---------------------------------------------------------------------------
// `fill(i, dst_device, size, stream)` copies buffer i's `size` bytes into the
// pre-allocated device pointer `dst_device`. It abstracts the byte source so the
// same reconstruction serves both the file reader (contiguous host payload) and
// the in-memory reader (a caller-owned, possibly multi-block pinned payload).
using leaf_buffer_fill =
  std::function<void(std::size_t i, void* dst_device, std::size_t size, rmm::cuda_stream_view)>;

static std::unique_ptr<compressed_representation> rep_from_leaf_desc(
  leaf_desc const& ld,
  cudf::size_type col_num_rows,
  leaf_buffer_fill const& fill,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr,
  std::string* err)
{
  auto const& bufs = ld.buffers;

  // Helper: allocate a device column and fill it from the i-th payload buffer.
  auto make_col = [&](std::size_t i) -> std::unique_ptr<cudf::column> {
    auto const& bd     = bufs[i];
    cudf::data_type dt = tag_to_dtype(bd.type_tag);
    auto col           = cudf::make_numeric_column(
      dt, static_cast<cudf::size_type>(bd.num_rows), cudf::mask_state::UNALLOCATED, stream, mr);
    if (bd.size_bytes > 0) {
      fill(i, col->mutable_view().head<void>(), static_cast<std::size_t>(bd.size_bytes), stream);
    }
    return col;
  };

  // Delta, Rle, For and Zigzag are codegen-only operators: encode always
  // produces a JIT codegen_fused_representation carrying the fused region's
  // manifest buffers, so their leaves are reconstructed unconditionally as a
  // fused rep.
  // A fused rep's num_rows drives the codegen decode grid. Use the node's own
  // output length (round-tripped in leaf_desc); fall back to the column row count
  // only for legacy blobs that predate the field (ld.num_rows == 0).
  const cudf::size_type node_rows =
    ld.num_rows > 0 ? static_cast<cudf::size_type>(ld.num_rows) : col_num_rows;
  auto make_fused_rep = [&](const char* kind_tag) {
    auto rep = std::make_unique<codegen_fused_representation>(
      kind_tag, tag_to_dtype(ld.type_tag), node_rows);
    for (std::size_t i = 0; i < bufs.size(); ++i) {
      rep->buffers.emplace_back(bufs[i].name, make_col(i));
    }
    return std::unique_ptr<compressed_representation>(std::move(rep));
  };

  if (ld.kind == OpId::Delta) { return make_fused_rep("delta"); }
  if (ld.kind == OpId::Rle) { return make_fused_rep("rle"); }
  if (ld.kind == OpId::For) { return make_fused_rep("for"); }
  if (ld.kind == OpId::Zigzag) { return make_fused_rep("zigzag"); }
  if (ld.kind == OpId::Identity) {
    bool is_fused = !(bufs.size() == 1 && bufs[0].name == "data");
    if (is_fused) return make_fused_rep("RawFused");
  }

  // All other kinds (and non-fused identity): route through reconstruct_representation.
  if (ld.kind == OpId::Unknown) {
    if (err) *err = "rep_from_leaf_desc: unknown leaf kind in file";
    return nullptr;
  }
  std::string_view cname = op_info(ld.kind).name;
  if (cname.empty()) {
    if (err)
      *err = "rep_from_leaf_desc: unsupported kind " + std::to_string(static_cast<int>(ld.kind));
    return nullptr;
  }
  std::vector<std::string> names;
  std::vector<std::unique_ptr<cudf::column>> cols;
  names.reserve(bufs.size());
  cols.reserve(bufs.size());
  for (std::size_t i = 0; i < bufs.size(); ++i) {
    names.push_back(bufs[i].name);
    cols.push_back(make_col(i));
  }
  return reconstruct_representation(
    std::string(cname), names, std::move(cols), stream, mr, err, ld.meta, ld.num_rows);
}

static constexpr std::uint8_t kVersion = 10;

[[nodiscard]] static bool exceeds_u16(std::size_t value) noexcept
{
  return value > std::numeric_limits<std::uint16_t>::max();
}

[[nodiscard]] static bool exceeds_u8(std::size_t value) noexcept
{
  return value > std::numeric_limits<std::uint8_t>::max();
}

// Validate every container/string whose size is narrowed into the v10 wire
// format before emitting any bytes. Keeping this separate from push_node keeps
// serialization infallible after the preflight (apart from host allocation).
[[nodiscard]] static std::string validate_v10_structure_widths(compressed_table const& table)
{
  if (exceeds_u16(table.columns.size())) {
    return "v10 column count exceeds the uint16 wire-format limit";
  }

  for (std::size_t column_index = 0; column_index < table.columns.size(); ++column_index) {
    auto const& column = table.columns[column_index];
    auto const prefix  = "column " + std::to_string(column_index);
    if (column.name.has_value() && exceeds_u16(column.name->size())) {
      return prefix + " name length exceeds the uint16 wire-format limit";
    }
    if (!column.compound) continue;

    auto const& nodes = column.compound->nodes;
    if (exceeds_u16(nodes.size())) {
      return prefix + " plan-node count exceeds the uint16 wire-format limit";
    }
    for (std::size_t node_index = 0; node_index < nodes.size(); ++node_index) {
      auto const& node       = nodes[node_index];
      auto const node_prefix = prefix + ", node " + std::to_string(node_index);
      if (exceeds_u16(node.op.size())) {
        return node_prefix + " operator-name length exceeds the uint16 wire-format limit";
      }
      if (node.attrs.bitjoin.has_value()) {
        auto const& inputs = node.attrs.bitjoin->inputs;
        if (exceeds_u16(inputs.size())) {
          return node_prefix + " bitjoin-input count exceeds the uint16 wire-format limit";
        }
        for (std::size_t input_index = 0; input_index < inputs.size(); ++input_index) {
          if (exceeds_u16(inputs[input_index].channel.size())) {
            return node_prefix + ", bitjoin input " + std::to_string(input_index) +
                   " channel-name length exceeds the uint16 wire-format limit";
          }
        }
      }
      if (exceeds_u16(node.children.size())) {
        return node_prefix + " edge count exceeds the uint16 wire-format limit";
      }
      for (std::size_t edge_index = 0; edge_index < node.children.size(); ++edge_index) {
        if (exceeds_u16(node.children[edge_index].channel.size())) {
          return node_prefix + ", edge " + std::to_string(edge_index) +
                 " channel-name length exceeds the uint16 wire-format limit";
        }
      }
      if (exceeds_u16(node.output_names.size())) {
        return node_prefix + " output count exceeds the uint16 wire-format limit";
      }
      for (std::size_t output_index = 0; output_index < node.output_names.size(); ++output_index) {
        if (exceeds_u16(node.output_names[output_index].size())) {
          return node_prefix + ", output " + std::to_string(output_index) +
                 " name length exceeds the uint16 wire-format limit";
        }
      }
    }
  }
  return {};
}

[[nodiscard]] static std::string validate_v10_description_widths(
  std::vector<std::vector<leaf_desc>> const& all_descs)
{
  for (std::size_t column_index = 0; column_index < all_descs.size(); ++column_index) {
    auto const& descs     = all_descs[column_index];
    auto const col_prefix = "column " + std::to_string(column_index);
    if (exceeds_u16(descs.size())) {
      return col_prefix + " leaf count exceeds the uint16 wire-format limit";
    }
    for (std::size_t leaf_index = 0; leaf_index < descs.size(); ++leaf_index) {
      auto const& leaf       = descs[leaf_index];
      auto const leaf_prefix = col_prefix + ", leaf " + std::to_string(leaf_index);
      if (exceeds_u8(leaf.buffers.size())) {
        return leaf_prefix + " buffer count exceeds the uint8 wire-format limit";
      }
      for (std::size_t buffer_index = 0; buffer_index < leaf.buffers.size(); ++buffer_index) {
        if (exceeds_u16(leaf.buffers[buffer_index].name.size())) {
          return leaf_prefix + ", buffer " + std::to_string(buffer_index) +
                 " name length exceeds the uint16 wire-format limit";
        }
      }
    }
  }
  return {};
}

// Serialize one node's structure (op, bitjoin params, edges, output names).
// Other ops carry their params in the op name, so only bitjoin needs attrs.
static void push_node(std::vector<std::uint8_t>& hdr, PlanNode const& node)
{
  push_str16(hdr, node.op);
  if (node.attrs.bitjoin.has_value()) {
    auto const& bj = *node.attrs.bitjoin;
    // Flags are read back as std::uint8_t — write them at that exact width.
    // (push_le deduces T from the argument, so a bare `1`/`0` would emit a
    // 4-byte int and desync the reader.)
    push_le(hdr, std::uint8_t{1});
    push_le(hdr, dtype_to_tag(bj.output_type));
    push_le(hdr, static_cast<std::uint16_t>(bj.inputs.size()));
    for (auto const& in : bj.inputs) {
      push_le(hdr, in.node);
      push_str16(hdr, in.channel);
      push_le(hdr, static_cast<std::uint8_t>(in.range.has_value() ? 1 : 0));
      if (in.range.has_value()) {
        push_le(hdr, in.range->first);
        push_le(hdr, in.range->second);
      }
    }
  } else {
    push_le(hdr, std::uint8_t{0});
  }
  push_le(hdr, static_cast<std::uint16_t>(node.children.size()));
  for (auto const& e : node.children) {
    push_str16(hdr, e.channel);
    push_le(hdr, e.child);
  }
  push_le(hdr, static_cast<std::uint16_t>(node.output_names.size()));
  for (auto const& n : node.output_names)
    push_str16(hdr, n);
}

// Inverse of push_node. output_paths mirror output_names as the node-local
// channel key; bitjoin input arity/ranges live on the bitjoin attrs, and each
// node's structural input_sources are rebuilt by compute_input_sources after
// the whole tree is read.
static bool read_node(Reader& r, PlanNode& node)
{
  if (!r.read_str16(node.op)) return false;
  std::uint8_t is_bitjoin;
  if (!r.read_le(is_bitjoin) || is_bitjoin > 1) return false;
  if (is_bitjoin) {
    bitjoin_attrs bj;
    std::uint8_t ot;
    if (!r.read_le(ot) || !is_valid_type_tag(ot)) return false;
    bj.output_type = tag_to_dtype(ot);
    std::uint16_t nin;
    if (!r.read_le(nin)) return false;
    bj.inputs.resize(nin);
    for (auto& in : bj.inputs) {
      if (!r.read_le(in.node) || !r.read_str16(in.channel)) return false;
      std::uint8_t has_range;
      if (!r.read_le(has_range) || has_range > 1) return false;
      if (has_range) {
        std::uint32_t hi, lo;
        if (!r.read_le(hi) || !r.read_le(lo)) return false;
        in.range = bit_range{hi, lo};
      }
    }
    node.attrs.bitjoin = std::move(bj);
  }
  std::uint16_t ne;
  if (!r.read_le(ne)) return false;
  node.children.resize(ne);
  for (auto& e : node.children) {
    if (!r.read_str16(e.channel) || !r.read_le(e.child)) return false;
  }
  std::uint16_t no;
  if (!r.read_le(no)) return false;
  node.output_names.resize(no);
  node.output_paths.resize(no);
  for (std::uint16_t i = 0; i < no; ++i) {
    if (!r.read_str16(node.output_names[i])) return false;
    node.output_paths[i] = node.output_names[i];
  }
  return true;
}

// Per-column parsed structure (header only; buffer bytes live in the payload
// region and are pulled in separately during reconstruction).
struct ColRecord {
  std::string name;
  std::uint8_t dtype_tag = 0;
  std::int32_t scale     = 0;  // fixed-point scale for the column dtype (0 otherwise)
  std::int64_t num_rows  = 0;
  PlanTree tree;
  std::vector<leaf_desc> leaf_descs;
  std::vector<std::vector<std::uint64_t>> buf_offsets;  // [leaf][buffer] -> payload offset
};

static bool validate_col_records(std::vector<ColRecord>& records, std::string* err)
{
  auto bad = [&](std::string message) {
    if (err) *err = std::move(message);
    return false;
  };

  for (std::size_t column_index = 0; column_index < records.size(); ++column_index) {
    auto& record            = records[column_index];
    auto const column_label = "column " + std::to_string(column_index);

    if (!is_valid_type_tag(record.dtype_tag)) {
      return bad(column_label + ": invalid column type tag " + std::to_string(record.dtype_tag));
    }
    if (record.num_rows < 0 || !fits_cudf_size_type(static_cast<std::uint64_t>(record.num_rows))) {
      return bad(column_label + ": row count is outside cudf::size_type");
    }
    auto const column_rows = static_cast<std::uint64_t>(record.num_rows);

    auto const& nodes = record.tree.nodes;
    if (!nodes.empty() && nodes.front().op != "input") {
      return bad(column_label + ": plan node 0 is not the input node");
    }

    std::vector<std::size_t> incoming_edges(nodes.size(), 0);
    for (std::size_t node_index = 0; node_index < nodes.size(); ++node_index) {
      auto const& node = nodes[node_index];
      if (node.output_names.size() >
          static_cast<std::size_t>(std::numeric_limits<ChannelId>::max()) + 1) {
        return bad(column_label + ": plan node " + std::to_string(node_index) +
                   " has too many output channels");
      }

      auto const has_output = [](PlanNode const& producer, std::string const& channel) {
        return std::find(producer.output_names.begin(), producer.output_names.end(), channel) !=
               producer.output_names.end();
      };

      for (auto const& edge : node.children) {
        if (edge.child <= node_index || edge.child >= nodes.size()) {
          return bad(column_label + ": plan edge from node " + std::to_string(node_index) +
                     " has an invalid child index");
        }
        if (node_index != 0 && !has_output(node, edge.channel)) {
          return bad(column_label + ": plan edge from node " + std::to_string(node_index) +
                     " names an unknown output channel");
        }
        ++incoming_edges[edge.child];
      }

      if (node.attrs.bitjoin.has_value()) {
        for (auto const& input : node.attrs.bitjoin->inputs) {
          if (input.node >= node_index) {
            return bad(column_label + ": bitjoin node " + std::to_string(node_index) +
                       " has an invalid input node index");
          }
          if (input.node != 0 && !has_output(nodes[input.node], input.channel)) {
            return bad(column_label + ": bitjoin node " + std::to_string(node_index) +
                       " names an unknown input channel");
          }
          auto const& producer     = nodes[input.node];
          auto const matching_edge = std::find_if(
            producer.children.begin(), producer.children.end(), [&](PlanEdge const& edge) {
              return edge.child == node_index && edge.channel == input.channel;
            });
          if (matching_edge == producer.children.end()) {
            return bad(column_label + ": bitjoin node " + std::to_string(node_index) +
                       " input is inconsistent with the plan edges");
          }
        }
      }
    }

    for (std::size_t node_index = 1; node_index < nodes.size(); ++node_index) {
      auto const& node = nodes[node_index];
      auto const expected_inputs =
        node.attrs.bitjoin.has_value() ? node.attrs.bitjoin->inputs.size() : std::size_t{1};
      if (incoming_edges[node_index] != expected_inputs) {
        return bad(column_label + ": plan node " + std::to_string(node_index) +
                   " has an invalid incoming-edge count");
      }
    }

    std::unordered_set<std::uint64_t> leaf_destinations;
    for (std::size_t leaf_index = 0; leaf_index < record.leaf_descs.size(); ++leaf_index) {
      auto& leaf            = record.leaf_descs[leaf_index];
      auto const leaf_label = column_label + ", leaf " + std::to_string(leaf_index);

      if (!is_serialized_leaf_kind(leaf.kind)) {
        return bad(leaf_label + ": invalid serialized leaf kind");
      }
      if (!is_valid_type_tag(leaf.type_tag)) {
        return bad(leaf_label + ": invalid leaf type tag " + std::to_string(leaf.type_tag));
      }
      if (!fits_cudf_size_type(leaf.num_rows)) {
        return bad(leaf_label + ": row count is outside cudf::size_type");
      }
      if (leaf.node_index == 0 || leaf.node_index >= nodes.size()) {
        return bad(leaf_label + ": node index does not name a non-input plan node");
      }

      auto const& node = nodes[leaf.node_index];
      if (leaf.slot != kSelfRepSlot &&
          (leaf.slot < 0 || static_cast<std::size_t>(leaf.slot) >= node.output_paths.size())) {
        return bad(leaf_label + ": slot is out of range");
      }

      auto const destination =
        (static_cast<std::uint64_t>(leaf.node_index) << 32) | static_cast<std::uint32_t>(leaf.slot);
      if (!leaf_destinations.insert(destination).second) {
        return bad(leaf_label + ": duplicate leaf destination");
      }

      if (leaf.slot == kSelfRepSlot) {
        auto const node_kind = op_id_from_name(node.op);
        if (!node_kind.has_value() || *node_kind != leaf.kind) {
          return bad(leaf_label + ": leaf kind is inconsistent with the owning node");
        }
      }

      if (leaf.slot == kSelfRepSlot && leaf.num_rows != 0 && !nodes.empty()) {
        auto const root_edge = std::find_if(
          nodes.front().children.begin(), nodes.front().children.end(), [&](PlanEdge const& edge) {
            return edge.child == leaf.node_index;
          });
        if (root_edge != nodes.front().children.end() && leaf.num_rows != column_rows) {
          return bad(leaf_label + ": root leaf row count disagrees with the column");
        }
      }

      if (record.buf_offsets[leaf_index].size() != leaf.buffers.size()) {
        return bad(leaf_label + ": buffer-offset count is inconsistent");
      }

      for (std::size_t buffer_index = 0; buffer_index < leaf.buffers.size(); ++buffer_index) {
        auto& buffer            = leaf.buffers[buffer_index];
        auto const buffer_label = leaf_label + ", buffer " + std::to_string(buffer_index);

        if (!is_valid_type_tag(buffer.type_tag)) {
          return bad(buffer_label + ": invalid type tag " + std::to_string(buffer.type_tag));
        }

        auto const buffer_type = tag_to_dtype(buffer.type_tag);
        if (buffer_type.id() == cudf::type_id::STRING) {
          return bad(buffer_label + ": STRING is not a fixed-width payload type");
        }
        if (!std::in_range<std::size_t>(buffer.size_bytes)) {
          return bad(buffer_label + ": byte size does not fit std::size_t");
        }
        if (!std::in_range<std::size_t>(record.buf_offsets[leaf_index][buffer_index])) {
          return bad(buffer_label + ": payload offset does not fit std::size_t");
        }

        auto const element_size = static_cast<std::uint64_t>(cudf::size_of(buffer_type));
        if (buffer.size_bytes % element_size != 0) {
          return bad(buffer_label + ": byte size is not divisible by the element size");
        }

        buffer.num_rows = buffer.size_bytes / element_size;
        if (!fits_cudf_size_type(buffer.num_rows)) {
          return bad(buffer_label + ": derived row count is outside cudf::size_type");
        }
      }

      bool const is_plain_identity = leaf.kind == OpId::Identity && leaf.buffers.size() == 1 &&
                                     leaf.buffers.front().name == "data";
      if (is_plain_identity) {
        auto const& data = leaf.buffers.front();
        if (data.type_tag != leaf.type_tag) {
          return bad(leaf_label + ": identity data type disagrees with the leaf type");
        }
        if (leaf.num_rows != 0 && data.num_rows != leaf.num_rows) {
          return bad(leaf_label + ": identity buffer row count disagrees with the leaf");
        }
        if (leaf.num_rows == 0 && leaf.slot == kSelfRepSlot) {
          auto const root_edge =
            std::find_if(nodes.front().children.begin(),
                         nodes.front().children.end(),
                         [&](PlanEdge const& edge) { return edge.child == leaf.node_index; });
          if (root_edge != nodes.front().children.end() && data.num_rows != column_rows) {
            return bad(leaf_label + ": identity buffer row count disagrees with the column");
          }
        }
      }
    }
  }

  return true;
}

// Parse the .hpln v10 header from `r` into one ColRecord per column. On success
// `r` is left pointing just past the header (i.e. at the payload region for the
// concatenated file layout). Returns false and sets *err on any structural error.
static bool parse_hpln_header(Reader& r, std::vector<ColRecord>& out, std::string* err)
{
  auto bad = [&](std::string const& m) {
    if (err) *err = m;
    return false;
  };

  std::uint8_t magic[4];
  if (!r.read(magic, 4)) return bad("truncated header");
  if (magic[0] != 'H' || magic[1] != 'P' || magic[2] != 'L' || magic[3] != 'N')
    return bad("not a HPLN file");
  std::uint8_t ver;
  if (!r.read_le(ver)) return bad("truncated header");
  if (ver != kVersion)
    return bad("unsupported version " + std::to_string(ver) + " (expected " +
               std::to_string(kVersion) + ")");

  std::uint16_t num_cols;
  if (!r.read_le(num_cols)) return bad("truncated header");
  out.clear();
  out.resize(num_cols);

  for (std::uint16_t ci = 0; ci < num_cols; ++ci) {
    auto& cr = out[ci];
    if (!r.read_str16(cr.name)) return bad("truncated col name");
    if (!r.read_le(cr.dtype_tag)) return bad("truncated col dtype");
    if (!r.read_le(cr.scale)) return bad("truncated col scale");
    if (!r.read_le(cr.num_rows)) return bad("truncated col num_rows");

    std::uint16_t nn;
    if (!r.read_le(nn)) return bad("truncated num_nodes");
    cr.tree.nodes.resize(nn);
    for (std::uint16_t ni = 0; ni < nn; ++ni) {
      if (!read_node(r, cr.tree.nodes[ni])) return bad("truncated plan node");
    }

    std::uint16_t nl;
    if (!r.read_le(nl)) return bad("truncated num_leaves");
    cr.leaf_descs.resize(nl);
    cr.buf_offsets.resize(nl);

    for (std::uint16_t li = 0; li < nl; ++li) {
      auto& ld = cr.leaf_descs[li];
      if (!r.read_le(ld.node_index)) return bad("truncated leaf node_index");
      if (!r.read_le(ld.slot)) return bad("truncated leaf slot");
      std::uint8_t k;
      if (!r.read_le(k)) return bad("truncated leaf kind");
      ld.kind = static_cast<OpId>(k);
      if (!r.read_le(ld.type_tag)) return bad("truncated leaf type_tag");
      if (!r.read_le(ld.num_rows)) return bad("truncated leaf num_rows");
      if (!read_meta(r, ld.meta)) return bad("truncated/unknown leaf meta");

      std::uint8_t nb;
      if (!r.read_le(nb)) return bad("truncated num_bufs");
      ld.buffers.resize(nb);
      cr.buf_offsets[li].resize(nb);

      for (std::uint8_t bi = 0; bi < nb; ++bi) {
        auto& bd = ld.buffers[bi];
        if (!r.read_str16(bd.name)) return bad("truncated buf name");
        if (!r.read_le(bd.type_tag)) return bad("truncated buf type_tag");
        if (!r.read_le(bd.size_bytes)) return bad("truncated buf size_bytes");
        std::uint64_t poff;
        if (!r.read_le(poff)) return bad("truncated buf payload_offset");
        cr.buf_offsets[li][bi] = poff;
      }
    }
  }
  return validate_col_records(out, err);
}

// Reconstruct a compressed_table from parsed column records, pulling each leaf
// buffer's bytes into device memory via `fetch(offset, size, dst_device, stream)`.
// `recs` is consumed (plan trees are moved into the result).
static compressed_table reconstruct_from_records(
  std::vector<ColRecord>& recs,
  payload_fetch_fn const& fetch,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr,
  std::string* err,
  std::optional<std::span<const std::size_t>> selected_columns = std::nullopt)
{
  auto fail = [&](std::string const& message) -> compressed_table {
    if (err) *err = message;
    return {};
  };

  if (selected_columns.has_value()) {
    std::vector<bool> seen(recs.size(), false);
    for (std::size_t const index : *selected_columns) {
      if (index >= recs.size()) {
        return fail("selected column index " + std::to_string(index) + " is out of range");
      }
      if (seen[index]) {
        return fail("selected column index " + std::to_string(index) + " is duplicated");
      }
      seen[index] = true;
    }
  }

  std::size_t const output_columns =
    selected_columns.has_value() ? selected_columns->size() : recs.size();
  compressed_table result;
  result.columns.resize(output_columns);

  for (std::size_t output_index = 0; output_index < output_columns; ++output_index) {
    std::size_t const source_index =
      selected_columns.has_value() ? (*selected_columns)[output_index] : output_index;
    auto& record  = recs[source_index];
    auto& out_col = result.columns[output_index];

    if (!record.name.empty()) out_col.name = record.name;
    auto const dtype_id = tag_to_dtype(record.dtype_tag).id();
    out_col.dtype = (dtype_id == cudf::type_id::DECIMAL32 || dtype_id == cudf::type_id::DECIMAL64 ||
                     dtype_id == cudf::type_id::DECIMAL128)
                      ? cudf::data_type{dtype_id, record.scale}
                      : cudf::data_type{dtype_id};
    out_col.num_rows = record.num_rows;

    if (record.tree.nodes.empty()) continue;

    auto compound = std::make_unique<PlanTree>();
    *compound     = std::move(record.tree);
    auto& nodes   = compound->nodes;

    for (std::size_t leaf_index = 0; leaf_index < record.leaf_descs.size(); ++leaf_index) {
      auto const& leaf    = record.leaf_descs[leaf_index];
      auto const& offsets = record.buf_offsets[leaf_index];
      if (leaf.node_index >= nodes.size()) {
        return fail("leaf node_index out of range in col " + std::to_string(source_index));
      }

      auto fill = [&](std::size_t buffer_index,
                      void* destination,
                      std::size_t size,
                      rmm::cuda_stream_view fill_stream) {
        fetch(offsets[buffer_index], size, destination, fill_stream);
      };

      std::string rep_error;
      auto rep = rep_from_leaf_desc(
        leaf, static_cast<cudf::size_type>(record.num_rows), fill, stream, mr, &rep_error);
      if (!rep) {
        return fail("rep_from_leaf_desc (col " + std::to_string(source_index) + "): " + rep_error);
      }

      PlanNode& node = nodes[leaf.node_index];
      if (leaf.slot == kSelfRepSlot) {
        node.meta = rep->describe_meta();
        node.rep  = std::move(rep);
      } else if (leaf.slot >= 0 && static_cast<std::size_t>(leaf.slot) < node.output_paths.size()) {
        node.channels.emplace(node.output_paths[leaf.slot], std::move(rep));
      } else {
        return fail("leaf slot out of range in col " + std::to_string(source_index));
      }
    }

    compute_input_sources(*compound);
    out_col.compound = std::move(compound);
  }

  return result;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

std::string write_compressed_table(compressed_table const& table,
                                   std::string const& path,
                                   rmm::cuda_stream_view stream)
{
  // Build the header + payload buffer list once (shared with the in-memory
  // writer), then gather the payload into one contiguous blob for the file.
  std::vector<std::uint8_t> hdr;
  std::vector<payload_buffer_ref> buffers;
  std::uint64_t payload_bytes = 0;
  std::string err = build_compressed_table_header(table, hdr, buffers, payload_bytes, stream);
  if (!err.empty()) return err;

  if (!std::in_range<std::size_t>(payload_bytes)) {
    return "compressed payload size does not fit std::size_t";
  }
  std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_bytes));
  std::string copy_error;
  for (auto const& b : buffers) {
    if (b.size_bytes > 0 && b.device_ptr) {
      auto const status = cudaMemcpyAsync(payload.data() + static_cast<std::size_t>(b.offset),
                                          b.device_ptr,
                                          static_cast<std::size_t>(b.size_bytes),
                                          cudaMemcpyDeviceToHost,
                                          stream.value());
      if (status != cudaSuccess) {
        copy_error =
          "device-to-host payload copy enqueue failed: " + std::string(cudaGetErrorString(status));
        break;
      }
    }
  }
  stream.synchronize();  // D→H copies must complete before the file write

  if (!copy_error.empty()) return copy_error;
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return "failed to open '" + path + "' for writing";
  f.write(reinterpret_cast<const char*>(hdr.data()), static_cast<std::streamsize>(hdr.size()));
  f.write(reinterpret_cast<const char*>(payload.data()),
          static_cast<std::streamsize>(payload.size()));
  if (!f) return "write error on '" + path + "'";
  return {};
}

compressed_table read_compressed_table(std::string const& path,
                                       rmm::cuda_stream_view stream,
                                       rmm::device_async_resource_ref mr,
                                       std::string* error_out)
{
  auto fail = [&](std::string const& msg) -> compressed_table {
    if (error_out) *error_out = msg;
    return {};
  };

  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return fail("failed to open '" + path + "' for reading");
  auto const end_position = f.tellg();
  if (end_position == std::streampos{-1}) {
    return fail("failed to determine size of '" + path + "'");
  }
  auto const end_offset = static_cast<std::streamoff>(end_position);
  if (end_offset < 0 || !std::in_range<std::size_t>(end_offset)) {
    return fail("size of '" + path + "' does not fit std::size_t");
  }
  auto const file_size = static_cast<std::size_t>(end_offset);
  if (std::cmp_greater(file_size, std::numeric_limits<std::streamsize>::max())) {
    return fail("size of '" + path + "' exceeds the stream read limit");
  }
  f.seekg(0);
  if (!f) return fail("failed to seek to the beginning of '" + path + "'");
  std::vector<std::uint8_t> raw(file_size);
  f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(file_size));
  if (!f) return fail("read error on '" + path + "'");

  Reader r{raw.data(), file_size};
  std::vector<ColRecord> col_records;
  if (!parse_hpln_header(r, col_records, error_out)) return {};

  // The payload region is whatever follows the header in the file.
  const std::uint8_t* payload_base = r.p;
  const std::size_t payload_total  = r.rem;

  // Bounds-check every buffer up front so a truncated file is reported here
  // rather than faulting mid-copy, then copy each buffer straight to device.
  for (auto const& cr : col_records) {
    for (std::size_t li = 0; li < cr.leaf_descs.size(); ++li) {
      for (std::size_t bi = 0; bi < cr.leaf_descs[li].buffers.size(); ++bi) {
        auto const offset = static_cast<std::size_t>(cr.buf_offsets[li][bi]);
        auto const size   = static_cast<std::size_t>(cr.leaf_descs[li].buffers[bi].size_bytes);
        if (offset > payload_total || size > payload_total - offset) {
          return fail("payload out of bounds");
        }
      }
    }
  }

  payload_fetch_fn fetch =
    [&](std::uint64_t offset, std::size_t size, void* destination, rmm::cuda_stream_view s) {
      auto const status = cudaMemcpyAsync(destination,
                                          payload_base + static_cast<std::size_t>(offset),
                                          size,
                                          cudaMemcpyHostToDevice,
                                          s.value());
      if (status != cudaSuccess) { throw std::runtime_error(cudaGetErrorString(status)); }
    };

  return reconstruct_from_records(col_records, fetch, stream, mr, error_out);
}

// ─── In-memory (pinned host) serialization ──────────────────────────────────

std::string build_compressed_table_header(compressed_table const& table,
                                          std::vector<std::uint8_t>& out_header,
                                          std::vector<payload_buffer_ref>& out_buffers,
                                          std::uint64_t& out_payload_bytes,
                                          rmm::cuda_stream_view stream)
{
  out_header.clear();
  out_buffers.clear();
  out_payload_bytes = 0;

  if (auto const error = validate_v10_structure_widths(table); !error.empty()) return error;

  auto const all_descs = table.describe(stream);
  if (auto const error = validate_v10_description_widths(all_descs); !error.empty()) return error;

  // A STRING column is physically split across offsets and chars (plus an
  // optional null mask), so no single parent head() pointer describes its
  // bytes. Reject identity STRING and any unexpected raw STRING channel before
  // exposing a fabricated contiguous payload range.
  for (auto const& descs : all_descs) {
    for (auto const& desc : descs) {
      if (desc.kind == OpId::Identity &&
          tag_to_dtype(desc.type_tag).id() == cudf::type_id::STRING) {
        return "identity STRING leaves are not serializable; use str_split";
      }
      for (auto const& buffer : desc.buffers) {
        if (tag_to_dtype(buffer.type_tag).id() == cudf::type_id::STRING) {
          return "raw STRING payload channels are not serializable; decompose into offsets and "
                 "chars";
        }
      }
    }
  }

  auto& hdr = out_header;
  hdr.push_back('H');
  hdr.push_back('P');
  hdr.push_back('L');
  hdr.push_back('N');
  push_le(hdr, kVersion);
  push_le(hdr, static_cast<std::uint16_t>(table.columns.size()));

  std::uint64_t payload_offset = 0;

  static const PlanTree kEmptyTree;
  for (std::size_t ci = 0; ci < table.columns.size(); ++ci) {
    auto const& col   = table.columns[ci];
    auto const& descs = all_descs[ci];

    push_str16(hdr, col.name.value_or(std::string{}));
    push_le(hdr, dtype_to_tag(col.dtype));
    push_le(hdr, col.dtype.scale());  // fixed-point scale (0 for non-decimal)
    push_le(hdr, col.num_rows);

    // Structural plan tree (identical layout to the file header, so the same
    // parser reconstructs it): the node array is the source of truth on read.
    PlanTree const& tree = col.compound ? *col.compound : kEmptyTree;
    push_le(hdr, static_cast<std::uint16_t>(tree.nodes.size()));
    for (auto const& node : tree.nodes)
      push_node(hdr, node);

    push_le(hdr, static_cast<std::uint16_t>(descs.size()));
    for (auto const& ld : descs) {
      push_le(hdr, ld.node_index);
      push_le(hdr, ld.slot);
      push_le(hdr, static_cast<std::uint8_t>(ld.kind));
      push_le(hdr, ld.type_tag);
      push_le(hdr, ld.num_rows);
      push_meta(hdr, ld.meta);
      push_le(hdr, static_cast<std::uint8_t>(ld.buffers.size()));

      for (auto const& bd : ld.buffers) {
        push_str16(hdr, bd.name);
        push_le(hdr, bd.type_tag);
        push_le(hdr, bd.size_bytes);
        push_le(hdr, payload_offset);

        // Record the buffer for the caller to stage out of device memory; no
        // bytes are copied here.
        out_buffers.push_back(payload_buffer_ref{payload_offset, bd.device_ptr, bd.size_bytes});
        payload_offset += bd.size_bytes;
      }
    }
  }

  out_payload_bytes = payload_offset;
  return {};
}

std::string build_compressed_table_header(compressed_table_inspection table,
                                          std::vector<std::uint8_t>& out_header,
                                          std::vector<payload_buffer_ref>& out_buffers,
                                          std::uint64_t& out_payload_bytes)
{
  return build_compressed_table_header(
    *table.table_, out_header, out_buffers, out_payload_bytes, table.stream_);
}

compressed_table read_compressed_table_from_memory(
  std::span<const std::uint8_t> header,
  payload_fetch_fn const& fetch,
  rmm::cuda_stream_view stream,
  rmm::device_async_resource_ref mr,
  std::string* error_out,
  std::optional<std::span<const std::size_t>> selected_columns)
{
  Reader reader{header.data(), header.size()};
  std::vector<ColRecord> column_records;
  if (!parse_hpln_header(reader, column_records, error_out)) return {};
  return reconstruct_from_records(column_records, fetch, stream, mr, error_out, selected_columns);
}

}  // namespace simpatico
