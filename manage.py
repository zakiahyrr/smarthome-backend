#!/usr/bin/env python3
import argparse
from getpass import getpass
from werkzeug.security import generate_password_hash
from app import DB_AKTIF, get_db

def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    admin = sub.add_parser("create-admin")
    admin.add_argument("--username", required=True)
    admin.add_argument("--email", required=True)
    args = parser.parse_args()
    password = getpass("Password admin (minimal 12 karakter): ")
    if len(password) < 12 or not DB_AKTIF:
        raise SystemExit("Password atau database tidak valid")
    conn = get_db(); cur = conn.cursor()
    cur.execute("INSERT INTO users (username,email,password) VALUES (%s,%s,%s)", (args.username, args.email, generate_password_hash(password)))
    conn.commit(); cur.close(); conn.close()

if __name__ == "__main__":
    main()
