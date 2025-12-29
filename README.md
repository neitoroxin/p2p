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

## Build with cmake

```bash
git clone https://github.com/neitoroxin/p2p.git
cd p2p
rm -rf build
mkdir build
cd build
cmake ..
cmake --build .
```

### start backend on your server
```bash
./server
```

### start frontend on your pc
```bash
./client
```

## How to use

on client you can see your nodeid:
![alt text](https://github.com/neitoroxin/p2p/blob/main/raw/image-1.png)
on another client connect using command:
```bash
connect <another_node_id>
```
make sure the connection is correct using command `list`:
![alt text](https://github.com/neitoroxin/p2p/blob/main/raw/image-2.png)

send message using command `send <node_id> "message"`
![alt text](https://github.com/neitoroxin/p2p/blob/main/raw/image-3.png)

disconnect server, and enjoy :)