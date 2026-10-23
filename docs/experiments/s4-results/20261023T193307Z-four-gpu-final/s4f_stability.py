import pathlib,sys,json,hashlib,os
ROOT=pathlib.Path('/home/ubuntu/sirius/build/s4-final-validation');E=ROOT/'docs/experiments/s4-results/20261023T193307Z-four-gpu-final';sys.path.insert(0,str(ROOT/'scripts'));from run_s4_experiments import source_identity
checks=[]
for label,role,kind,source in [('prerequisite','baseline','prerequisite',ROOT/'build/reference-prerequisite'),('final','final','prerequisite',ROOT),('control','baseline','s4_control',ROOT/'build/reference-control')]:
 saved=json.loads((E/'build-records'/label/'manifest.json').read_text());actual=source_identity(source,role,kind);assert actual==saved['source'],label;checks.append({'revision':label,'source_identity_unchanged':True,'source_sha256':saved['source_sha256']})
pkgs=[]
for p in sorted(pathlib.Path(os.environ['CONDA_PREFIX'],'conda-meta').glob('*.json')):
 d=json.loads(p.read_text());pkgs.append({k:d.get(k) for k in ('name','version','build','channel','sha256')})
assert pkgs==json.loads((E/'installed-packages.json').read_text())
fetch=json.loads((E/'build-environment.json').read_text())['reused_fetch_sources']
for item in fetch:
 p=pathlib.Path(item['path']);files=sorted(f for f in p.rglob('*') if f.is_file() and '.git' not in f.relative_to(p).parts);hashes=[{'path':str(f.relative_to(p)),'sha256':hashlib.sha256(f.read_bytes()).hexdigest()} for f in files];assert hashlib.sha256(json.dumps(hashes,sort_keys=True).encode()).hexdigest()==item['source_tree_sha256'],str(p)
configs=[]
for plan in json.loads((E/'planned-campaigns.json').read_text())['campaigns']:
 p=ROOT/plan['argv'][plan['argv'].index('--output')+1];configs.append(json.loads((p/'experiment.json').read_text())['config']['sha256'])
assert len(set(configs))==1
(E/'dependency-stability-audit.json').write_text(json.dumps({'status':'passed','revisions':checks,'installed_package_metadata_unchanged':True,'reused_fetch_source_trees_unchanged':True,'all_campaign_config_sha256':configs[0]},indent=2)+'\n');print('All source identities, dependency/package/fetch provenance and four campaign configurations match.')
