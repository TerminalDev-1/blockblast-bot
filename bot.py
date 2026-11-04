import argparse, gzip, struct, subprocess, time, sys, re, xml.etree.ElementTree as ET
from functools import lru_cache
import cv2
import planner
import adbwifi
import numpy as np
W,H=2880,1800
GX,GY,STEP=994,321,112
TRAYS=[(990,1290),(1290,1590),(1590,1890)]
# Measured from screen recordings: a held piece moves about 1.47 px for every
# 1 px of finger movement on both axes, and when lifted from the tray its centre
# jumps to this fixed height (independent of the piece or touch point).
GAIN=1.47
PICKUP_Y=1232
FULL=(1<<64)-1
RETRY=round(STEP/GAIN)  # finger distance that moves a piece by one cell

SLOW=1  # screenshot timeouts are stretched over Wi-Fi (set in __main__)

def adb(*args,timeout=20):
    return subprocess.run(['adb',*map(str,args)],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=timeout).stdout

def _capture_fast():
    # Raw pixels compressed on the tablet are ~2.5x faster than PNG.
    data=gzip.decompress(adb('exec-out','screencap | gzip -1',timeout=4*SLOW))
    w,h=struct.unpack('<II',data[:8]); header=len(data)-w*h*4
    if (w,h)!=(W,H) or not 12<=header<=32: raise ValueError('unexpected raw screenshot')
    return np.frombuffer(data,np.uint8,offset=header).reshape(H,W,4)[:,:,2::-1].copy()

def _capture_png():
    im=cv2.imdecode(np.frombuffer(adb('exec-out','screencap','-p',timeout=6*SLOW),np.uint8),cv2.IMREAD_COLOR)
    if im is None or im.shape[:2]!=(H,W): raise RuntimeError('Expected a 2880x1800 landscape screenshot.')
    return im

def capture():
    """Screenshot that never stalls for long: screencap occasionally hangs on the
    tablet, so each attempt is time-limited (the hung process is killed) and retried."""
    for attempt,grab in enumerate((_capture_fast,_capture_fast,_capture_png,_capture_fast,_capture_png)):
        try: return grab()
        except (subprocess.TimeoutExpired,subprocess.CalledProcessError,OSError,EOFError,ValueError,struct.error) as e:
            print(time.strftime('%H:%M:%S'),f'Screenshot attempt {attempt+1} failed ({type(e).__name__}); retrying.',flush=True)
    raise RuntimeError('Screenshots keep failing.')

def cell_stats(im):
    """Per-cell (lip, Lab colour, centre texture).
    lip: blocks in many themes are raised with a thick darker bottom edge; empty cells
    are flat or sunken. The row profile is smoothed over 6 px to ignore grid lines."""
    g=cv2.cvtColor(im,cv2.COLOR_BGR2GRAY).astype(np.float32); kernel=np.ones(6,np.float32)/6
    lab=cv2.cvtColor(im[GY:GY+8*STEP,GX:GX+8*STEP],cv2.COLOR_BGR2LAB).astype(np.float32)
    stats=[]
    for y in range(8):
        for x in range(8):
            x0=GX+x*STEP; y0=GY+y*STEP
            rows=g[y0:y0+STEP,x0+30:x0+82].mean(1)
            smooth=np.convolve(rows,kernel,'valid')
            lip=float(rows[30:70].mean()-smooth[76:STEP-6].min())
            centre=lab[y*STEP+30:y*STEP+82,x*STEP+30:x*STEP+82]
            stats.append((lip,centre.reshape(-1,3).mean(0),float(centre[:,:,0].std())))
    return stats

def colour_groups(colours,radius=20):
    """Greedy clusters of cells with near-identical colour, largest first."""
    d=np.linalg.norm(colours[:,None]-colours[None],axis=2); left=set(range(len(colours))); groups=[]
    while left:
        seed=max(left,key=lambda k:sum(d[k,j]<radius for j in left))
        g=[j for j in left if d[seed,j]<radius]; left-=set(g); groups.append(g)
    return sorted(groups,key=len,reverse=True)

def is_board(im):
    stats=cell_stats(im)
    # Board cells have flat centres (blocks and empty alike); menus and pop-ups don't.
    # A few textured cells are allowed for line-clear sparkles and combo text.
    if sum(t>12 for _,_,t in stats)>8: return False
    return len(colour_groups(np.array([c for _,c,_ in stats]))[0])>=4

EMPTY_COLOUR=None  # Lab colour of an empty cell in the current theme

def read_board(im,hint_empty=0):
    """Empty cells all share one colour; everything else is a block.
    The colour is learned rather than hard-coded because the theme changes during a run
    (dark, pink, mint, white, wood, ice...). hint_empty marks cells known to be empty
    (e.g. from the previous move), which re-learns the colour after a theme change."""
    global EMPTY_COLOUR
    stats=cell_stats(im); colours=np.array([c for _,c,_ in stats]); lips=np.array([l for l,_,_ in stats])
    empty=None
    known=[i for i in range(64) if hint_empty>>i&1]
    if len(known)>=3:
        cand=np.median(colours[known],axis=0)
        if np.mean(np.linalg.norm(colours[known]-cand,axis=1)<20)>=.8: empty=cand
    if empty is None and EMPTY_COLOUR is not None and np.sum(np.linalg.norm(colours-EMPTY_COLOUR,axis=1)<20)>=3:
        empty=EMPTY_COLOUR
    if empty is None:
        # Fresh start or new theme: prefer a large group without raised edges.
        groups=[g for g in colour_groups(colours) if len(g)>=3] or colour_groups(colours)
        best=min(groups,key=lambda g:(lips[g].mean()>25,-len(g)))
        empty=np.median(colours[best],axis=0)
    is_empty=np.linalg.norm(colours-empty,axis=1)<22
    EMPTY_COLOUR=np.median(colours[is_empty],axis=0) if is_empty.any() else empty
    return sum(1<<i for i in range(64) if not is_empty[i])

def green_button(im):
    hsv=cv2.cvtColor(im[1270:1460,1140:1740],cv2.COLOR_BGR2HSV)
    return np.mean((hsv[:,:,0]>35)&(hsv[:,:,0]<90)&(hsv[:,:,1]>100)&(hsv[:,:,2]>110))>.30

def play_button(im):
    # The results screen uses a white triangular play icon, including on its rainbow button.
    hsv=cv2.cvtColor(im[1260:1460,1250:1630],cv2.COLOR_BGR2HSV)
    mask=((hsv[:,:,2]>200)&(hsv[:,:,1]<60)).astype(np.uint8)
    _,_,stats,_=cv2.connectedComponentsWithStats(mask,8)
    return any(65<=w<=110 and 75<=h<=110 and 3000<=area<=7000
               for x,y,w,h,area in stats[1:])

def rating_popup(im):
    a=im[490:505,1080:1200].mean((0,1)); b=im[510:550,1750:1800].mean((0,1))
    return a[0]>150 and a[0]>a[2]*1.15 and b.mean()>110 and not is_board(im)

def close_accessible_ad():
    try:
        adb('shell','uiautomator','dump','/sdcard/bot-window.xml')
        xml=adb('shell','cat','/sdcard/bot-window.xml').decode('utf-8','replace')
        root=ET.fromstring(xml[xml.find('<?xml'):])
        for node in root.iter('node'):
            label=(node.get('text','')+' '+node.get('content-desc','')).strip().lower()
            if label not in ('close','dismiss','skip','close ad','dismiss ad','close interstitial','skip ad','x'): continue
            bounds=re.fullmatch(r'\[(\d+),(\d+)\]\[(\d+),(\d+)\]',node.get('bounds',''))
            if not bounds: continue
            x1,y1,x2,y2=map(int,bounds.groups())
            if x2>x1 and y2>y1 and y1<1620:
                print('Closing ad:',label,flush=True); adb('shell','input','tap',(x1+x2)//2,(y1+y2)//2); return True
    except (ValueError,ET.ParseError,subprocess.CalledProcessError): pass
    return False

def recover_screen(im,unknown_count):
    focus=adb('shell','dumpsys','window').decode('utf-8','replace')
    current=next((line for line in focus.splitlines() if 'mCurrentFocus=' in line),'')
    if current and 'com.block.juggle/' not in current:
        print('Returning from another app or ad.',flush=True); adb('shell','input','keyevent','BACK'); time.sleep(2); return
    if play_button(im):
        print('Result screen: starting next attempt against the current best.',flush=True); adb('shell','input','tap',1440,1335); time.sleep(5); return
    if green_button(im):
        print('Starting or restarting Classic.',flush=True); adb('shell','input','tap',1440,1370); time.sleep(4); return
    if rating_popup(im):
        print('Closing rating prompt.',flush=True); adb('shell','input','tap',1778,525); time.sleep(2); return
    if close_accessible_ad(): time.sleep(3); return
    print('Unknown screen: waiting to preserve the current game.',flush=True); time.sleep(10)

def _slot_piece(im,slot,threshold,background):
    """Read one tray slot at a given threshold: None (empty), a piece, or 'bad'."""
    left,right=TRAYS[slot]
    crop=im[1320:1610,left:right]
    mask=(np.linalg.norm(crop.astype(np.float32)-background,axis=2)>threshold).astype(np.uint8)
    n,labels,stats,centroids=cv2.connectedComponentsWithStats(mask,8)
    components=[(x,y,w,h,area) for x,y,w,h,area in stats[1:] if area>300 and w>=20 and h>=20]
    if not components: return None
    x0=min(a[0] for a in components); y0=min(a[1] for a in components)
    x1=max(a[0]+a[2] for a in components); y1=max(a[1]+a[3] for a in components)
    if x1-x0>275 or y1-y0>275: return 'bad'
    # Tray cells are 50 px; a real piece's box (plus a small shadow) fits that grid.
    if any(min(d%50,50-d%50)>14 for d in (x1-x0,y1-y0)): return 'bad'
    cells=[]
    for y in range(round((y1-y0)/50)):
        for x in range(round((x1-x0)/50)):
            patch=mask[y0+y*50+5:y0+y*50+45,x0+x*50+5:x0+x*50+45]
            if patch.size and np.mean(patch)>.40: cells.append((x,y))
    if not cells or len(cells)>9: return 'bad'
    return {'slot':slot,'cells':cells,'origin':(left+x0,1320+y0)}

def read_pieces(im):
    pieces=[]
    for slot,(left,right) in enumerate(TRAYS):
        # Background and its noise come from the strip just above the tray, which pieces
        # never reach. Some themes draw pieces close to the background colour (pink on
        # pink), others have textured backgrounds (wood), so the threshold adapts, and
        # other thresholds are tried before giving up.
        strip=im[1290:1325,left:right].astype(np.float32)
        background=np.median(strip.reshape(-1,3),axis=0)
        noise=float(np.percentile(np.linalg.norm(strip-background,axis=2),99))
        adaptive=min(45.0,max(18.0,noise*1.6+6))
        result='bad'
        for threshold in (adaptive,45,30,24,60,18,80):
            if threshold<noise*1.2: continue
            result=_slot_piece(im,slot,threshold,background)
            if result!='bad': break
        if result=='bad': raise RuntimeError(f'Unrecognized piece in tray {slot+1}.')
        if result: pieces.append(result)
    return pieces

def place(board,cells,ox,oy):
    mask=0
    for x,y in cells:
        x+=ox; y+=oy
        if x>=8 or y>=8: return None
        mask|=1<<(y*8+x)
    if mask&board: return None
    result=board|mask; lines=[]
    for y in range(8):
        m=255<<(8*y)
        if result&m==m: lines.append(m)
    for x in range(8):
        m=sum(1<<(y*8+x) for y in range(8))
        if result&m==m: lines.append(m)
    for m in lines: result &= ~m
    return result,len(lines)

SQUARES3=[sum(1<<((y+dy)*8+x+dx) for dy in range(3) for dx in range(3)) for y in range(6) for x in range(6)]
LINES5=[sum(1<<(y*8+x+i) for i in range(5)) for y in range(8) for x in range(4)] + [sum(1<<((y+i)*8+x) for i in range(5)) for y in range(4) for x in range(8)]

@lru_cache(maxsize=300000)
def quality(board):
    filled=board.bit_count(); isolated=0; adjacent=0
    for y in range(8):
        for x in range(8):
            if board>>(y*8+x)&1: continue
            neighbors=[(a,b) for a,b in [(x-1,y),(x+1,y),(x,y-1),(x,y+1)] if 0<=a<8 and 0<=b<8]
            occupied=sum(bool(board>>(b*8+a)&1) for a,b in neighbors)
            isolated+=occupied==len(neighbors); adjacent+=occupied
    square_room=sum(not (board&m) for m in SQUARES3)
    line_room=sum(not (board&m) for m in LINES5)
    return -filled*1.5-isolated*14-adjacent*.08+min(square_room,8)*.65+min(line_room,12)*.18

def choose(board,pieces,avoid=frozenset()):
    states=[(0,board,[],pieces)]
    for depth in range(len(pieces)):
        new=[]
        for score,b,moves,remaining in states:
            for i,p in enumerate(remaining):
                for y in range(8):
                    for x in range(8):
                        if not moves and (p['slot'],x,y) in avoid: continue
                        outcome=place(b,p['cells'],x,y)
                        if outcome is not None:
                            nb,lines=outcome
                            new.append((score+lines*25,nb,moves+[(p,x,y,nb)],remaining[:i]+remaining[i+1:]))
        if not new: break
        new.sort(key=lambda s:s[0]+quality(s[1]),reverse=True)
        # Preserve distinct board states and several alternative first moves.
        states=[]; seen=set(); per_first={}
        for candidate in new:
            score,nb,moves,left=candidate
            state_key=(nb,tuple(p['slot'] for p in left))
            first=moves[0]
            first_key=(first[0]['slot'],first[1],first[2])
            if state_key in seen or per_first.get(first_key,0)>=12: continue
            seen.add(state_key); per_first[first_key]=per_first.get(first_key,0)+1
            states.append(candidate)
            if len(states)>=180: break
    if not states or not states[0][2]: return None
    return max(states,key=lambda s:(len(s[2]),s[0]+quality(s[1])))[2][0]

def drag_piece(sx, sy, dx, dy, ms):
    # A single continuous swipe; the game moves the held piece GAIN x the finger.
    adb('shell','input','swipe',round(sx),round(sy),round(dx),round(dy),ms,timeout=ms/1000+8)

def log(*parts):
    print(time.strftime('%H:%M:%S'),*parts,flush=True)

def show(board,pieces):
    for y in range(8): print(''.join('#' if board>>(y*8+x)&1 else '.' for x in range(8)))
    for p in pieces: print('tray',p['slot']+1,'cells',p['cells'])

def settle(before, expected, timeout=3.0):
    """Capture until the board shows the move's result. Returns (image, board) or (image, None) off-board."""
    end=time.time()+timeout; last=None; im=None
    while True:
        im=capture()
        if not is_board(im):
            # Usually a transient effect; only report off-board if it persists.
            if time.time()>end: return im,None
            last=None; time.sleep(.2); continue
        b=read_board(im,FULL&~(before|expected))
        if b==expected: return im,b
        # A changed board read twice in a row is final (clear animations differ from both).
        if b!=before and b==last: return im,b
        if time.time()>end: return im,b
        last=b

def save_unknown(im):
    import os
    os.makedirs('unknown-screens',exist_ok=True)
    files=sorted(os.listdir('unknown-screens'))
    if len(files)<30: cv2.imwrite(os.path.join('unknown-screens',time.strftime('%Y%m%d-%H%M%S')+'.png'),im)

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('--play',action='store_true'); ap.add_argument('--max-moves',type=int,default=1000)
    ap.add_argument('--pickup-y',type=int,default=PICKUP_Y); ap.add_argument('--swipe-ms',type=int,default=300)
    args=ap.parse_args()
    corrections={}; turn=0; unknown_count=0; failed_board=None; failed_moves=set()
    plan_seq=[]; im=None; started=time.time(); stuck_since=None; tray_errors=0
    while turn<args.max_moves:
        if im is None: im=capture()
        if not is_board(im):
            if not args.play: print('Not on the game board.'); return
            unknown_count+=1; plan_seq=[]; im=None
            # Never tap anything until the screen has been unrecognised for a few reads.
            if unknown_count<5: time.sleep(.4); continue
            if unknown_count==5: save_unknown(capture())
            recover_screen(capture(),unknown_count); continue
        unknown_count=0; board=read_board(im)
        try: pieces=read_pieces(im)
        except RuntimeError as e:
            tray_errors+=1
            if tray_errors==20: save_unknown(im)  # a new theme to calibrate
            log(str(e),'Waiting for screen to settle.'); time.sleep(.5); im=None; continue
        tray_errors=0
        if board!=failed_board: failed_board=board; failed_moves.clear()
        if not pieces: log('Waiting for pieces or game-over screen.'); time.sleep(1); im=None; plan_seq=[]; continue
        by_slot={p['slot']:p for p in pieces}
        # Keep following the tray's plan while the board matches it; otherwise re-plan.
        following=(plan_seq and plan_seq[0][0] in by_slot and plan_seq[0][4]==board
                   and plan_seq[0][5]==tuple(by_slot[plan_seq[0][0]]['cells']))
        if not following:
            # New tray pieces animate in and can be misread mid-animation;
            # plan only from two consecutive screenshots that agree.
            time.sleep(.15); im2=capture()
            try: same=(is_board(im2) and read_board(im2)==board and
                       [(p['slot'],p['cells']) for p in read_pieces(im2)]==[(p['slot'],p['cells']) for p in pieces])
            except RuntimeError: same=False
            if not same: im=im2; continue
            pieces=read_pieces(im2); by_slot={p['slot']:p for p in pieces}
            t=time.time()
            seq=planner.plan(board,[(p['slot'],p['cells']) for p in pieces],failed_moves,seed=turn)
            plan_seq=[]; prev=board
            for slot,x,y,nb in seq: plan_seq.append((slot,x,y,nb,prev,tuple(by_slot[slot]['cells']))); prev=nb
            log(f'Planned {len(seq)} of {len(pieces)} pieces in {time.time()-t:.2f}s')
        turn+=1; print(f'\nMove {turn}  ({turn/max(1e-9,time.time()-started)*60:.0f} moves/min)',flush=True); show(board,pieces)
        if not plan_seq:
            if failed_moves:
                log('No untried placements left; re-reading the screen.'); failed_moves.clear(); time.sleep(1)
            else:
                # The game normally shows its result screen; if it never comes the screen
                # is being misread, so save it for calibration and try the normal recovery.
                stuck_since=stuck_since or time.time()
                if time.time()-stuck_since>30:
                    log('No legal moves for 30s without a game-over screen; trying recovery.')
                    save_unknown(im); recover_screen(capture(),99); stuck_since=None
                else:
                    log('No legal moves; waiting for game-over screen.'); time.sleep(2)
            im=None; continue
        stuck_since=None
        slot,x,y,expected,_,_=plan_seq.pop(0); p=by_slot[slot]
        log(f"Selected tray {slot+1}, grid column {x+1}, row {y+1}")
        if not args.play: return
        cx,cy=p['cells'][0]; ox,oy=p['origin']; sx=ox+cx*50+25; sy=oy+cy*50+25
        width=max(a for a,b in p['cells'])+1; height=max(b for a,b in p['cells'])+1
        target_x=GX+(x+width/2)*STEP; tray_center_x=ox+width*25; key=tuple(p['cells'])
        adjust_x,adjust_y=corrections.get(key,(0,0))
        target_y=GY+(y+height/2)*STEP
        dx=sx+(target_x-tray_center_x)/GAIN+adjust_x
        dy=sy+(target_y-args.pickup_y)/GAIN+adjust_y
        actual=board
        for retry_x,retry_y in [(0,0),(0,RETRY),(0,-RETRY),(RETRY,0),(-RETRY,0)]:
            # Each retry shifts the piece by one cell; never shift it off the board.
            if x==0 and retry_x<0: continue
            if x+width==8 and retry_x>0: continue
            if y==0 and retry_y<0: continue
            if y+height==8 and retry_y>0: continue
            drag_piece(sx,sy,dx+retry_x,dy+retry_y,args.swipe_ms)
            im,actual=settle(board,expected)
            if actual is None: log('Screen changed; checking state.'); break
            if actual!=board: break
            log('Retrying unplaced piece.')
        if actual is None: im=None; plan_seq=[]; continue
        if actual==board:
            failed_moves.add((slot,x,y)); plan_seq=[]
            log('Placement failed; trying a different move.'); continue
        if actual!=expected:
            plan_seq=[]
            matches=[(xx,yy) for yy in range(8) for xx in range(8) if (result:=place(board,p['cells'],xx,yy)) is not None and result[0]==actual]
            if len(matches)==1:
                ax,ay=matches[0]
                cx_=max(-1.5*RETRY,min(1.5*RETRY,adjust_x+retry_x+(x-ax)*STEP/GAIN))
                cy_=max(-1.5*RETRY,min(1.5*RETRY,adjust_y+retry_y+(y-ay)*STEP/GAIN))
                corrections[key]=(cx_,cy_)
                log(f'Piece landed at ({ax+1}, {ay+1}); recalibrated.')
            else:
                log('Could not verify placement; re-reading.'); time.sleep(.5); im=None

def pick_device():
    global SLOW
    SLOW=3 if adbwifi.is_wireless(adbwifi.ensure()) else 1

if __name__=='__main__':
    while True:
        try:
            pick_device()
            main()
            break
        except KeyboardInterrupt:
            print('\nStopped.', flush=True)
            break
        except Exception as e:
            # Keep playing for hours: log any error, wait for the tablet, and start over.
            import traceback; traceback.print_exc()
            print(f'Error: {e!r}. Rereading the screen in 5 seconds.', file=sys.stderr, flush=True)
            if '--play' not in sys.argv:
                sys.exit(1)
            time.sleep(5)
            try: pick_device()  # USB or Wi-Fi, reconnecting if needed
            except Exception: pass




