import json, math, sys
maps = {'sandstorm':None,'kanji':None,'subzero':None,'undergrowth':None,'oasis':None,'freight':None,'lostworld':None,'citadel':None,'industry':None}
out = {}
for m in maps:
    d = json.load(open(f'/home/user/src/KrunkNative/assets/maps/311_{m}.json'))
    px, py, pz = d['camPos']
    # map centre from object bounds
    xs=[o['p'][0] for o in d['objects'] if 'p' in o]; zs=[o['p'][2] for o in d['objects'] if 'p' in o]
    cx=(min(xs)+max(xs))/2; cz=(min(zs)+max(zs))/2
    yaw = math.atan2(-(cx-px), -(cz-pz))
    out[m] = [
        {"name":"horizon","pos":[px,py,pz],"pitch":0.05,"yaw":round(yaw,4)},
        {"name":"up","pos":[px,py,pz],"pitch":0.45,"yaw":round(yaw+2.0,4)},
        {"name":"down","pos":[px,py,pz],"pitch":-0.35,"yaw":round(yaw-1.6,4)},
    ]
json.dump(out, open('/home/user/src/KrunkNative/doc/ref/krunker-v3.1.1/sky/poses.json','w'), indent=1)
print(json.dumps(out))
