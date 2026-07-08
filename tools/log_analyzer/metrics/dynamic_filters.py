"""Parse Track-A dynamic-filter events and INFO query summaries."""

import json
import re
from typing import List

from .. import patterns
from ..validators import FormatWarnings


COLUMNS = [
    "timestamp",
    "kind",
    "event",
    "publication_plan_id",
    "target_id",
    "channel_id",
    "filter_id",
    "fields_json",
]

_FIELD_RE = re.compile(r"(?P<key>[a-z_]+)=(?P<value>\[[^\]]*\]|\S+)")


def _integer(fields: dict, *names: str):
    for name in names:
        value = fields.get(name)
        if value is not None and str(value).lstrip("-").isdigit():
            return int(value)
    return None


def parse(lines: List[str], warnings: FormatWarnings) -> List[dict]:
    rows: List[dict] = []
    for line in lines:
        kind = None
        regex = None
        if patterns.DYNF_SUMMARY_ANCHOR in line:
            kind, regex = "summary", patterns.DYNF_SUMMARY_RE
        elif patterns.DYNF_EVENT_ANCHOR in line:
            kind, regex = "event", patterns.DYNF_EVENT_RE
        else:
            continue
        match = regex.search(line)
        if not match:
            warnings.record_drift(f"dynamic_filter_{kind}", line)
            continue
        fields = {
            m.group("key"): m.group("value")
            for m in _FIELD_RE.finditer(match.group("fields"))
        }
        rows.append(
            {
                "timestamp": match.group("ts"),
                "kind": kind,
                "event": match.group("event"),
                "publication_plan_id": _integer(fields, "pub_plan"),
                "target_id": _integer(fields, "target"),
                "channel_id": _integer(fields, "channel"),
                "filter_id": _integer(fields, "filter"),
                "fields_json": json.dumps(fields, sort_keys=True),
            }
        )
    return rows
