import sys, json, base64
json.dump({'format': 'sf2/base64', 'description': 'TimGM6mb General MIDI soundfont by Tim Brechbill (GPL v2)',
           'data': base64.b64encode(open(sys.argv[1], 'rb').read()).decode()}, open(sys.argv[2], 'w'))
