#!/usr/bin/env python3
"""Generate PartyMic's original SVG, PNG and multi-resolution ICO. Dev dependency: Pillow."""
from pathlib import Path
from PIL import Image, ImageDraw
import math

ROOT = Path(__file__).resolve().parents[1] / 'src/windows/assets'
SCALE = 4
SIZE = 256
image = Image.new('RGBA', (SIZE*SCALE, SIZE*SCALE))
mask = Image.new('L', image.size)
ImageDraw.Draw(mask).rounded_rectangle((12*SCALE, 12*SCALE, 244*SCALE, 244*SCALE), 56*SCALE, fill=255)
gradient = Image.new('RGBA', image.size)
pixels = gradient.load()
for y in range(image.height):
    t = y/(image.height-1)
    for x in range(image.width):
        pixels[x,y] = (round(255*(1-t)+234*t), round(151*(1-t)+88*t), round(55*(1-t)+12*t), 255)
image.paste(gradient, (0,0), mask)
draw = ImageDraw.Draw(image)
svg = ['<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">',
       '<title>PartyMic — microphone and music</title>',
       '<defs><linearGradient id="orange" x2="0" y2="1"><stop stop-color="#ff9737"/><stop offset="1" stop-color="#ea580c"/></linearGradient></defs>',
       '<rect x="12" y="12" width="232" height="232" rx="56" fill="url(#orange)"/>']
def capsule(x,y,w,h,fill):
    draw.rounded_rectangle(tuple(v*SCALE for v in (x,y,x+w,y+h)), min(w,h)/2*SCALE, fill)
    svg.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{min(w,h)/2}" fill="{fill}"/>')
def stroke(points,width,fill):
    scaled=[(round(x*SCALE),round(y*SCALE)) for x,y in points]
    draw.line(scaled, fill=fill, width=width*SCALE, joint='curve')
    r=width*SCALE/2
    for x,y in scaled: draw.ellipse((x-r,y-r,x+r,y+r),fill)
    svg.append('<polyline points="'+' '.join(f'{x:.3f},{y:.3f}' for x,y in points)+f'" fill="none" stroke="{fill}" stroke-width="{width}" stroke-linecap="round" stroke-linejoin="round"/>')
capsule(107,54,42,88,'#ffffff')
points=[(91,119),(91,136)]+[(128+37*math.cos(math.pi-i*math.pi/40),136+37*math.sin(math.pi-i*math.pi/40)) for i in range(41)]+[(165,119)]
stroke(points,12,'#ffffff')
stroke([(128,173),(128,195)],12,'#ffffff')
stroke([(108,197),(148,197)],12,'#ffffff')
for x,top,bottom in [(58,107,131),(76,91,147),(180,91,147),(198,107,131)]:
    stroke([(x,top),(x,bottom)],8,'#ffffff')
# Teal corner accent links the app mark to the selected state in the UI.
draw.ellipse((184*SCALE,184*SCALE,226*SCALE,226*SCALE),fill='#ffffff')
draw.ellipse((190*SCALE,190*SCALE,220*SCALE,220*SCALE),fill='#0d9488')
svg += ['<circle cx="205" cy="205" r="21" fill="#ffffff"/>','<circle cx="205" cy="205" r="15" fill="#0d9488"/>','</svg>']
ROOT.mkdir(parents=True,exist_ok=True)
(ROOT/'partymic.svg').write_text('\n'.join(svg)+'\n')
image=image.resize((256,256),Image.Resampling.LANCZOS)
image.save(ROOT/'partymic.png')
image.save(ROOT/'partymic.ico',sizes=[(n,n) for n in (16,20,24,32,40,48,64,96,128,256)])
print('Generated SVG, PNG and 10-size ICO in',ROOT)
