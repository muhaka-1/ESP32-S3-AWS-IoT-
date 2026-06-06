#!/usr/bin/env python3
"""
provision_certs.py – Write AWS IoT X.509 certs to ESP32 NVS partition.

Run once after creating a Thing in AWS IoT Console and downloading the
certificate bundle.

Usage:
    pip install esptool
    python scripts/provision_certs.py \
        --root-ca certs/AmazonRootCA1.pem \
        --cert    certs/device-cert.pem.crt \
        --key     certs/device-private.pem.key \
        --port    /dev/ttyUSB0
"""

import argparse
import subprocess
import struct
import os
import sys


NVS_PARTITION_SIZE = 0x6000   # 24 KB – must match partitions.csv
NVS_PAGE_SIZE      = 0x1000


def read_pem(path: str) -> bytes:
    with open(path, "rb") as f:
        data = f.read()
    if not data.endswith(b"\x00"):
        data += b"\x00"
    return data


def build_nvs_csv(root_ca: bytes, cert: bytes, key: bytes) -> str:
    """Generate nvs_partition_gen CSV for the three certificate blobs."""
    lines = [
        "key,type,encoding,value",
        "certs,namespace,,",
        f"root_ca,data,string,{root_ca.decode().strip()}",
        f"client_cert,data,string,{cert.decode().strip()}",
        f"client_key,data,string,{key.decode().strip()}",
    ]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description="Provision AWS IoT certs to ESP32 NVS")
    parser.add_argument("--root-ca", required=True)
    parser.add_argument("--cert",    required=True)
    parser.add_argument("--key",     required=True)
    parser.add_argument("--port",    default="/dev/ttyUSB0")
    parser.add_argument("--baud",    type=int, default=921600)
    parser.add_argument("--addr",    default="0x9000",
                        help="NVS partition offset (see partitions.csv)")
    args = parser.parse_args()

    print("[*] Reading PEM files...")
    root_ca = read_pem(args.root_ca)
    cert    = read_pem(args.cert)
    key     = read_pem(args.key)

    csv_path = "/tmp/certs_nvs.csv"
    bin_path = "/tmp/certs_nvs.bin"

    csv_content = build_nvs_csv(root_ca, cert, key)
    with open(csv_path, "w") as f:
        f.write(csv_content)

    idf_path = os.environ.get("IDF_PATH", "/opt/esp/idf")
    nvs_gen  = os.path.join(idf_path, "components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py")

    if not os.path.exists(nvs_gen):
        print(f"[!] nvs_partition_gen.py not found at {nvs_gen}")
        print("    Set IDF_PATH or run from inside the ESP-IDF environment.")
        sys.exit(1)

    print("[*] Generating NVS binary...")
    subprocess.run(
        ["python3", nvs_gen, "generate", csv_path, bin_path, str(NVS_PARTITION_SIZE)],
        check=True
    )

    print(f"[*] Flashing NVS to {args.addr} on {args.port}...")
    subprocess.run(
        ["esptool.py", "--port", args.port, "--baud", str(args.baud),
         "write_flash", args.addr, bin_path],
        check=True
    )

    print("[✓] Certificates provisioned successfully!")
    print("    Device will load certs from NVS on next boot.")


if __name__ == "__main__":
    main()
