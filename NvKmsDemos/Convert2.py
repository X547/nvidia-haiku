import sys
from itertools import groupby
import yaml
import pyaml

def convert_mode_list(modes):
	print('{')
	for mode in modes:
		if not mode['valid']:
			continue

		timings = mode['timings']
		print('\t{')
		print('\t\t.timing = {')
		print('\t\t\t.pixel_clock  = ' + str(timings['pixelClockHz'] // 1000) + ',')
		print('\t\t\t.h_display    = ' + str(timings['hVisible']) + ',')
		print('\t\t\t.h_sync_start = ' + str(timings['hSyncStart']) + ',')
		print('\t\t\t.h_sync_end   = ' + str(timings['hSyncEnd']) + ',')
		print('\t\t\t.h_total      = ' + str(timings['hTotal']) + ',')
		print('\t\t\t.v_display    = ' + str(timings['vVisible']) + ',')
		print('\t\t\t.v_sync_start = ' + str(timings['vSyncStart']) + ',')
		print('\t\t\t.v_sync_end   = ' + str(timings['vSyncEnd']) + ',')
		print('\t\t\t.v_total      = ' + str(timings['vTotal']) + ',')
		print('\t\t\t.flags        = 0' + (' | B_POSITIVE_HSYNC' if timings['hSyncPos'] else '') + (' | B_POSITIVE_VSYNC' if timings['vSyncPos'] else '') + (' | B_TIMING_INTERLACED' if timings['interlaced'] else '') + ',')
		print('\t\t},')
		print('\t\t.space = B_RGB32,')
		print('\t\t.virtual_width = ' + str(timings['hVisible']) + ',')
		print('\t\t.virtual_height = ' + str(timings['vVisible']) + ',')
		print('\t},')

	print('}')
	pass

with open('NvKmsInfo.yaml', 'r') as f:
	input = yaml.safe_load(f)

modes = input['dpys']['13']['modes']

convert_mode_list(modes)
