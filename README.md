# QmiSMS
## Usage
1. Copy the example configuration file and modify it as needed
```bash
cp config.example.yaml config.yaml
```
2. Run the container
```bash
docker run -d \
  --name qmi_sms_reader \
  --restart unless-stopped \
  --device /dev/cdc-wdm0:/dev/cdc-wdm0 \
  -v $(pwd)/config.yaml:/app/config.yaml \
  ghcr.io/pa733/qmisms:latest
```
## Compatible Servers
[Super SMS Bridge](https://github.com/PA733/SuperSMSBridge)