a simple P2P net

## Planning backend
Open port 55555 on your server
### UFW:
```bash
sudo ufw allow 55555/tcp
sudo ufw reload
```

### iptables:
```bash
sudo iptables -A INPUT -p tcp --dport 55555 -j ACCEPT
```

## How to use
on server download and start `server.py`:
```bash
git clone https://github.com/neitoroxin/p2p.git
cd p2p
python3 server.py
```

on client download and start 'client.py':
```bash
git clone https://github.com/neitoroxin/p2p.git
cd p2p
python3 client.py
```

then on client you can see your nodeid:
![alt text](image-1.png)
on another client connect using command:
```bash
connect <another_node_id>
```
make sure the connection is correct using command `list`:
![alt text](image-2.png)

send message using command `send <node_id> "message"`
![alt text](image-3.png)

disconnect server, and enjoy :)