#!/usr/bin/env python3
# client.py - P2P client with hole punching and direct confirmation
# Usage: python3 client.py
# IMPORTANT: set SERVER_IP to your server's public IP before running

import socket
import json
import time
import threading
import uuid
import sys

# Replace with your server public IP
SERVER_IP = '89.169.159.66'   # <-- change this
SERVER = (SERVER_IP, 55555)

NODE_ID = str(uuid.uuid4())[:8]

sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
sock.bind(('0.0.0.0', 0))
sock.settimeout(1.0)

peers = {}  # peer_id -> {'public_addr': (ip,port), 'direct_addr': (ip,port), 'status':..., 'direct_confirmed': bool, 'last_seen': ts}
running = True

def send_raw(addr, obj):
    try:
        sock.sendto(json.dumps(obj).encode(), addr)
    except Exception as e:
        print("send_raw error:", e)

def register_and_stun():
    # STUN
    send_raw(SERVER, {'type': 'stun', 'node_id': NODE_ID})
    time.sleep(0.1)
    # register
    send_raw(SERVER, {'type': 'register', 'node_id': NODE_ID, 'local_addr': [sock.getsockname()[0], sock.getsockname()[1]]})

def recv_loop():
    while running:
        try:
            data, addr = sock.recvfrom(4096)
            try:
                msg = json.loads(data.decode())
            except Exception:
                continue
            handle_msg(msg, addr)
        except socket.timeout:
            continue
        except Exception as e:
            if running:
                print("recv error:", e)
            break

def handle_msg(msg, addr):
    t = msg.get('type')
    if t == 'stun_response':
        print("STUN response:", msg.get('public_ip'), msg.get('public_port'))
    elif t == 'register_response':
        print("Registered on server. Active peers:", msg.get('active'))
    elif t == 'peer_info':
        pid = msg.get('peer_id')
        pub = tuple(msg.get('public', []))
        peers[pid] = peers.get(pid, {})
        peers[pid]['public_addr'] = pub
        peers[pid]['status'] = 'known'
        peers[pid]['last_seen'] = time.time()
        print(f"Peer info: {pid} -> {pub}")
    elif t == 'incoming_punch':
        from_node = msg.get('from_node')
        from_public = tuple(msg.get('from_public', []))
        print(f"Incoming punch from {from_node} at {from_public}")
        # reply immediately to the public address to help hole punching
        send_raw(from_public, {'type': 'hole_punch', 'node_id': NODE_ID, 'peer_id': from_node, 'timestamp': time.time()})
    elif t == 'punch_coordinates':
        peer_public = tuple(msg.get('peer_public', []))
        peer_id = msg.get('peer_id')
        print(f"Punch coordinates for {peer_id}: {peer_public}")
        # start active hole punching attempts
        threading.Thread(target=perform_hole_punch, args=(peer_id, peer_public), daemon=True).start()
    elif t == 'hole_punch':
        # received hole punch from other node
        peer_id = msg.get('node_id')
        print(f"Received hole_punch from {peer_id} via {addr}")
        # reply directly to sender address (addr) to establish mapping
        send_raw(addr, {'type': 'hole_punch_response', 'node_id': NODE_ID, 'peer_id': peer_id, 'timestamp': time.time()})
        # record direct_addr candidate
        peers[peer_id] = peers.get(peer_id, {})
        peers[peer_id]['direct_addr'] = addr
        peers[peer_id]['status'] = 'connected'
    elif t == 'hole_punch_response':
        print("Received hole_punch_response from", addr)
        # find which peer this corresponds to by matching addr to known peer public or direct
        matched = None
        for pid, info in peers.items():
            if info.get('public_addr') == addr or info.get('direct_addr') == addr:
                matched = pid
                break
        if matched:
            peers[matched]['direct_addr'] = addr
            peers[matched]['status'] = 'connected'
            # send test ping to confirm round-trip
            send_direct_test_ping(matched)
    elif t == 'message':
        sender = msg.get('from')
        content = msg.get('content')
        print(f"\nMessage from {sender}: {content} (addr {addr})")
        # if it's a direct test ping/pong, handle confirmation
        if content == "__direct_test_ping__":
            # reply with pong
            send_message(sender, "__direct_test_pong__")
        elif content == "__direct_test_pong__":
            if sender in peers:
                peers[sender]['direct_confirmed'] = True
                peers[sender]['last_seen'] = time.time()
                print(f"✅ Direct confirmed with {sender}")
    elif t == 'relayed':
        print(f"Relayed message from {msg.get('from')}: {msg.get('content')}")
    elif t == 'pong':
        # server pong
        pass
    else:
        # unknown or other messages
        # print("other msg:", msg)
        pass

def perform_hole_punch(peer_id, peer_public):
    """Active hole punching: send multiple UDP packets to peer_public and wait for response"""
    print(f"Starting hole punch to {peer_id} at {peer_public}")
    peers[peer_id] = peers.get(peer_id, {})
    peers[peer_id]['public_addr'] = peer_public
    peers[peer_id]['status'] = 'punching'
    encoded = json.dumps({'type': 'hole_punch', 'node_id': NODE_ID, 'peer_id': peer_id, 'timestamp': time.time()}).encode()
    for i in range(20):
        try:
            sock.sendto(encoded, peer_public)
        except Exception:
            pass
        # short wait to allow responses
        time.sleep(0.3)
        # if direct_addr already set and confirmed, stop
        if peers.get(peer_id, {}).get('direct_confirmed'):
            return
    print("Hole punch attempts finished for", peer_id)

def send_direct_test_ping(peer_id):
    """Send a test ping to peer to confirm direct connectivity"""
    info = peers.get(peer_id)
    if not info:
        return
    target = info.get('direct_addr') or info.get('public_addr')
    if not target:
        return
    msg = {'type': 'message', 'from': NODE_ID, 'to': peer_id, 'content': "__direct_test_ping__", 'timestamp': time.time()}
    try:
        sock.sendto(json.dumps(msg).encode(), tuple(target))
        # wait a bit for pong
    except Exception:
        pass

def send_message(peer_id, text):
    """Send message to peer: prefer direct confirmed, else try direct candidate, else relay via server"""
    info = peers.get(peer_id)
    if not info:
        print("Peer unknown. Use connect <peer_id> first.")
        return False

    # if direct confirmed, send directly
    if info.get('direct_confirmed') and info.get('direct_addr'):
        target = tuple(info['direct_addr'])
        try:
            msg = {'type': 'message', 'from': NODE_ID, 'to': peer_id, 'content': text, 'timestamp': time.time()}
            sock.sendto(json.dumps(msg).encode(), target)
            print(f"Sent direct to {peer_id} -> {target}")
            return True
        except Exception as e:
            print("Direct send failed:", e)

    # if we have a direct candidate (not yet confirmed), try it
    if info.get('direct_addr'):
        try:
            target = tuple(info['direct_addr'])
            msg = {'type': 'message', 'from': NODE_ID, 'to': peer_id, 'content': text, 'timestamp': time.time()}
            sock.sendto(json.dumps(msg).encode(), target)
            print(f"Sent to candidate direct {peer_id} -> {target}")
            return True
        except Exception:
            pass

    # fallback: send via server relay (server must be up)
    try:
        relay = {'type': 'relay', 'node_id': NODE_ID, 'to': peer_id, 'content': text}
        send_raw(SERVER, relay)
        print(f"Sent via server relay to {peer_id}")
        return True
    except Exception as e:
        print("Relay failed:", e)
        return False

def peer_keepalive_loop():
    """Keep NAT mapping alive for confirmed direct peers"""
    while running:
        for pid, info in list(peers.items()):
            if info.get('direct_confirmed') and info.get('direct_addr'):
                try:
                    ping = {'type': 'message', 'from': NODE_ID, 'to': pid, 'content': '__keepalive__', 'timestamp': time.time()}
                    sock.sendto(json.dumps(ping).encode(), tuple(info['direct_addr']))
                except Exception:
                    pass
        time.sleep(25)

def server_ping_loop():
    """Keep track of server availability"""
    while running:
        try:
            send_raw(SERVER, {'type': 'ping', 'node_id': NODE_ID})
        except:
            pass
        time.sleep(5)

def main_loop():
    print("="*50)
    print("P2P client")
    print("Your id:", NODE_ID)
    print("Make sure SERVER_IP in this file is set to your server's public IP")
    print("="*50)
    register_and_stun()

    # start threads
    threading.Thread(target=recv_loop, daemon=True).start()
    threading.Thread(target=peer_keepalive_loop, daemon=True).start()
    threading.Thread(target=server_ping_loop, daemon=True).start()

    try:
        while True:
            cmd = input("\n> ").strip()
            if not cmd:
                continue
            if cmd.startswith("connect "):
                peer = cmd.split(" ",1)[1].strip()
                # ask server for peer info and request punch
                send_raw(SERVER, {'type': 'get_peer', 'node_id': NODE_ID, 'peer_id': peer})
                time.sleep(0.1)
                send_raw(SERVER, {'type': 'request_punch', 'node_id': NODE_ID, 'peer_id': peer, 'local_addr': [sock.getsockname()[0], sock.getsockname()[1]]})
            elif cmd.startswith("send "):
                parts = cmd.split(" ",2)
                if len(parts) < 3:
                    print("Usage: send <peer_id> <message>")
                    continue
                peer, text = parts[1], parts[2]
                send_message(peer, text)
            elif cmd.startswith("send_relay "):
                parts = cmd.split(" ",2)
                if len(parts) < 3:
                    print("Usage: send_relay <peer_id> <message>")
                    continue
                peer, text = parts[1], parts[2]
                send_raw(SERVER, {'type': 'relay', 'node_id': NODE_ID, 'to': peer, 'content': text})
            elif cmd == "list":
                if not peers:
                    print("No known peers")
                else:
                    for pid, info in peers.items():
                        status = info.get('status','unknown')
                        direct = info.get('direct_confirmed', False)
                        pub = info.get('public_addr')
                        direct_addr = info.get('direct_addr')
                        print(f"{pid}: status={status} direct_confirmed={direct} public={pub} direct={direct_addr}")
            elif cmd == "quit":
                break
            else:
                print("Unknown command. Commands: connect, send, send_relay, list, quit")
    except KeyboardInterrupt:
        pass
    finally:
        global running
        running = False
        try:
            send_raw(SERVER, {'type': 'unregister', 'node_id': NODE_ID})
        except:
            pass
        sock.close()
        print("Client stopped")

if __name__ == "__main__":
    if 'YOUR_SERVER_IP' in SERVER_IP:
        print("ERROR: please set SERVER_IP in client.py to your server public IP and restart.")
        sys.exit(1)
    main_loop()
