import os
import select

REPORT_ID = 102
PAYLOAD_LEN = 61

def packet(cmd_id):
    return bytes([REPORT_ID, cmd_id] + [0] * (PAYLOAD_LEN - 1))

def send(fd, cmd_id, alias, timeout=1.5):
    os.write(fd, packet(cmd_id))
    while True:
        r, _, _ = select.select([fd], [], [], timeout)
        if not r:
            return None
        resp = os.read(fd, 64)
        if len(resp) >= 5 and resp[0] == REPORT_ID and resp[1] in (cmd_id, alias):
            return resp

def main():
    fd = os.open("/dev/hidraw0", os.O_RDWR)

    try:
        resp = send(fd, 137, 13)

        if resp is None:
            print("No response")
            return

        if resp[2] == 0 and resp[3] == 0:
            print("Off")
            return

        print(f"battery: {resp[4]}%")
    finally:
        os.close(fd)

if __name__ == "__main__":
    main()
