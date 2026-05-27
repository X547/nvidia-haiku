import sys
from itertools import groupby
import yaml
import pyaml

def mode_key(mode):
	return (mode['timings']['hVisible'], mode['timings']['vVisible'])

def mode_key2(mode):
	return (mode['timings']['RRx1k'], mode['timings']['interlaced'])

def build_mode_groups2(modes):
	modes = sorted(modes, key = mode_key2)

	groups = []
	for key, group in groupby(modes, mode_key2):
		groups.append({
			'refreshRate': key[0],
			'interlaced': key[1],
			'modes': list(group)
		})
	return groups

def build_mode_groups(modes):
	modes = sorted(modes, key = mode_key)

	groups = []
	for key, group in groupby(modes, mode_key):
		groups.append({
			'width': key[0],
			'height': key[1],
			'modes': build_mode_groups2(list(group))
		})
	return groups

def convert_dpys(dpys):
	outDpys = {}
	for dpyId, dpy in dpys.items():
		outDpy = dpy.copy()
		if 'modes' in dpy:
			outDpy['modes'] = build_mode_groups(dpy['modes'])
		outDpys[dpyId] = outDpy

	return outDpys

def convert_root(root):
	outRoot = root.copy()
	outRoot['dpys'] = convert_dpys(root['dpys'])
	return outRoot

with open('NvKmsInfo.yaml', 'r') as f:
	input = yaml.safe_load(f)

# modes = input['dpys']['13']['modes']

pyaml.dump(convert_root(input), sys.stdout, width=2048, sort_keys=False)
