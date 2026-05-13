# OK3588 GoAhead Web Service

GoAhead-based web service for the OK3588 device. The project provides a browser UI, image/video processing endpoints, model-management endpoints, device-status APIs, SQLite-backed login/register, and a systemd service for boot-time startup.

## Project Layout

```text
.
├── goahead_prj/        # Business web service, static web assets, API handlers
│   ├── src/            # Application C sources
│   ├── web/            # Built frontend assets served by GoAhead
│   ├── web_cfg/        # GoAhead route/auth config
│   └── Makefile
└── libgoahead/         # GoAhead + mbedtls library source
    ├── inc/
    ├── src/
    └── Makefile
```

## Features

- HTTP service on `8081`
- HTTPS service on `1443`
- Login endpoint: `/action/mylogin`
- Register endpoint: `/action/register`
- SQLite user database at runtime: `goahead_prj/data/users.db`
- Device status endpoint: `/action/overview`
- Image, video, model, and log APIs under `/action/*`
- Frontend registration patch loaded from `web/register-patch-v2.js`
- Optional systemd service: `goahead-codex.service`

## Build On Device

Run on the OK3588 device:

```bash
cd /home/forlinx/Desktop/GUO_teacher/codex-backup

cd libgoahead
make clean
make all

cd ../goahead_prj
mkdir -p lib
cp ../libgoahead/libgo.so ../libgoahead/libgo.a ./lib/

make clean
make all
```

## Run Manually

```bash
cd /home/forlinx/Desktop/GUO_teacher/codex-backup/goahead_prj
LD_LIBRARY_PATH=./lib ./goahead
```

Open:

```text
http://192.168.50.20:8081/index.html
```

## Login And Register

On first startup, the service creates the SQLite user table and a default account:

```text
username: root
password: root
```

Passwords are not stored as plaintext. They are hashed with GoAhead's password hashing helpers.

Register new users from the login page, or call:

```bash
curl -F username=testuser1 -F password=test1234 \
  http://127.0.0.1:8081/action/register
```

Login test:

```bash
curl -F username=root -F password=root \
  http://127.0.0.1:8081/action/mylogin
```

## Systemd Boot Service

Installed service name:

```text
goahead-codex.service
```

Useful commands:

```bash
sudo systemctl status goahead-codex.service
sudo systemctl restart goahead-codex.service
sudo systemctl stop goahead-codex.service
sudo systemctl enable goahead-codex.service
sudo systemctl disable goahead-codex.service
journalctl -u goahead-codex.service -f
```

Service file used on the device:

```ini
[Unit]
Description=GoAhead Codex Web Service
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=forlinx
Group=forlinx
WorkingDirectory=/home/forlinx/Desktop/GUO_teacher/codex-backup/goahead_prj
Environment=LD_LIBRARY_PATH=/home/forlinx/Desktop/GUO_teacher/codex-backup/goahead_prj/lib
ExecStart=/home/forlinx/Desktop/GUO_teacher/codex-backup/goahead_prj/goahead
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
```

## Verification

```bash
curl http://127.0.0.1:8081/action/overview
curl -F username=root -F password=root http://127.0.0.1:8081/action/mylogin
```

## Notes

Runtime files such as `users.db`, logs, PID files, uploaded videos/images, model files, private keys, and compiled binaries are intentionally ignored by Git.
