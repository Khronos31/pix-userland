"""Read only firmware state; does not authenticate, boot, tune or capture."""
import json,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'outputs/pix-dt295-20261015/pid273-raw-reference-03/candidate-v5'))
from dt295 import USB,Device
with USB() as usb:
 d=Device(usb)
 print(json.dumps({'firmware_state':d.word(0x82008),'state_notifications_pending':bool(d.word(0x90074)&4)}))
