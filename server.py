#!/usr/bin/env python3
# server.py - UDP coordinator for hole punching and optional relay
# Usage: python3 server.py

import socket
import json
import time
import threading

HOST = '0.0.0.0'
PORT = 55555

# node_id -> {'public': (ip,port), 'local': [ip,port], 'last': ts}
nodes = {}
lock = threading.Lock()

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind((HOST, PORT))
print(f"Server listening on {HOST}:{PORT}")

def send(addr, obj):
    try:
        sock.sendto(json.dumps(obj).encode(), addr)
    except Exception as e:
        print("send error:", e)

def handle_message(msg, addr):
    now = time.time()
    t = msg.get('type')
    nid = msg.get('node_id')

    # update registry if node_id present
    if nid:
        with lock:
            nodes[nid] = {
                'public': addr,
                'local': msg.get('local_addr', []),
                'last': now
            }

    print(f"[{time.strftime('%H:%M:%S')}] {addr} -> {t} ({nid})")

    if t == 'stun':
        # reply with observed public address
        send(addr, {'type': 'stun_response', 'node_id': nid, 'public_ip': addr[0], 'public_port': addr[1]})
        return

    if t == 'register':
        send(addr, {'type': 'register_response', 'status': 'ok', 'active': max(0, len(nodes)-1)})
        return

    if t == 'get_peer':
        peer = msg.get('peer_id')
        with lock:
            if peer in nodes:
                p = nodes[peer]
                send(addr, {'type': 'peer_info', 'peer_id': peer, 'public': list(p['public']), 'local': p.get('local', [])})
            else:
                send(addr, {'type': 'error', 'message': 'peer not found', 'available': list(nodes.keys())})
        return

    if t == 'request_punch':
        peer = msg.get('peer_id')
        with lock:
            if peer not in nodes:
                send(addr, {'type': 'error', 'message': 'peer not found'})
                return
            peer_addr = nodes[peer]['public']
        # notify peer about incoming attempt (so peer starts sending)
        notify = {
            'type': 'incoming_punch',
            'from_node': nid,
            'from_public': list(addr),
            'from_local': msg.get('local_addr', []),
            'timestamp': time.time()
        }
        send(peer_addr, notify)
        # send coordinates to initiator
        coords = {
            'type': 'punch_coordinates',
            'peer_id': peer,
            'peer_public': list(peer_addr),
            'timestamp': time.time()
        }
        send(addr, coords)
        return

    if t == 'relay':
        # simple relay: forward content to recipient via server
        to = msg.get('to')
        with lock:
            if to in nodes:
                relay = {
                    'type': 'relayed',
                    'from': nid,
                    'content': msg.get('content'),
                    'timestamp': time.time()
                }
                send(nodes[to]['public'], relay)
                send(addr, {'type': 'relay_ack', 'status': 'ok'})
            else:
                send(addr, {'type': 'error', 'message': 'recipient not found'})
        return

    # optional: server can forward hole_punch/hole_punch_response if clients ask
    if t in ('hole_punch', 'hole_punch_response'):
        peer = msg.get('peer_id')
        with lock:
            if peer in nodes:
                forward = msg.copy()
                forward['relayed_by_server'] = True
                send(tuple(nodes[peer]['public']), forward)
        return

    # ping/pong
    if t == 'ping':
        send(addr, {'type': 'pong', 'timestamp': time.time()})
        return

def cleanup_loop():
    while True:
        time.sleep(60)
        now = time.time()
        with lock:
            for k in list(nodes.keys()):
                if now - nodes[k]['last'] > 180:
                    print("Removing inactive node", k)
                    del nodes[k]

threading.Thread(target=cleanup_loop, daemon=True).start()

try:
    while True:
        data, addr = sock.recvfrom(4096)
        try:
            msg = json.loads(data.decode())
        except Exception:
            continue
        threading.Thread(target=handle_message, args=(msg, addr), daemon=True).start()
except KeyboardInterrupt:
    print("\nShutting down server")
finally:
    sock.close()