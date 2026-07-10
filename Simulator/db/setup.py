#!/usr/bin/env python3
"""
RUC 맵매칭 시뮬레이터 - DB 환경 구축 스크립트

  - roadnet 스키마 + roadnet.prim_rawgps 테이블 생성 (create_sim.sql 실행)
  - 도로망(network.moct_link) 존재 여부 및 좌표계(SRID) 확인
  - mytobcom 계정 접속 점검

다른 PC 에서도 동일 환경을 재현하기 위한 단일 진입점이다.

전제:
  - roadnet DB 가 이미 존재해야 한다 (roadnet/scripts/roadnet.sh 로 생성/적재).
  - 스키마/권한 생성은 슈퍼유저(postgres) 권한이 필요하다.

사용법:
  # 로컬 (peer 인증, sudo 사용)
  python3 Simulator/db/setup.py

  # 원격/패스워드 (TCP 접속)
  python3 Simulator/db/setup.py --host localhost --port 5432 --super-user postgres

  # 생성 후 mytobcom 접속/INSERT 권한까지 점검
  python3 Simulator/db/setup.py --check
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
DEFAULT_SQL = HERE / "create_sim.sql"


def find_psql() -> str:
    for cand in ("psql", "/usr/pgsql-18/bin/psql", "/usr/bin/psql"):
        p = shutil.which(cand) if not cand.startswith("/") else (cand if Path(cand).exists() else None)
        if p:
            return p
    sys.exit("[setup] psql 실행파일을 찾을 수 없습니다. PostgreSQL 클라이언트를 설치하세요.")


def run_psql_super(args: argparse.Namespace, sql_args: list[str]) -> int:
    """슈퍼유저로 psql 실행. host 미지정 시 sudo -u <super-user> 사용 (peer 인증)."""
    psql = find_psql()
    base = [psql, "-v", "ON_ERROR_STOP=1", "-d", args.dbname]
    if args.host:
        cmd = base + ["-U", args.super_user, "-h", args.host, "-p", str(args.port)] + sql_args
        env = os.environ.copy()
        if args.super_password:
            env["PGPASSWORD"] = args.super_password
        return subprocess.call(cmd, env=env)
    # peer 인증
    cmd = ["sudo", "-u", args.super_user, psql, "-v", "ON_ERROR_STOP=1", "-d", args.dbname] + sql_args
    return subprocess.call(cmd)


def check_as_mytobcom(args: argparse.Namespace) -> int:
    """mytobcom 계정으로 접속하여 도로망 SRID 및 INSERT 권한을 점검한다."""
    psql = find_psql()
    env = os.environ.copy()
    env["PGPASSWORD"] = args.app_password
    sql = (
        "SELECT 'moct_link_count' AS k, count(*)::text AS v FROM network.moct_link "
        "UNION ALL "
        "SELECT 'moct_link_srid', COALESCE(MIN(ST_SRID(geom))::text,'(none)') FROM network.moct_link "
        "UNION ALL "
        "SELECT 'prim_rawgps_exists', "
        "(to_regclass('roadnet.prim_rawgps') IS NOT NULL)::text;"
    )
    cmd = [psql, "-d", args.dbname, "-U", args.app_user,
           "-h", args.host or "localhost", "-p", str(args.port), "-c", sql]
    print(f"[check] {args.app_user}@{args.host or 'localhost'}:{args.port}/{args.dbname} 점검 ...")
    return subprocess.call(cmd, env=env)


def main() -> None:
    ap = argparse.ArgumentParser(description="RUC 맵매칭 시뮬레이터 DB 환경 구축")
    ap.add_argument("--dbname", default="roadnet", help="대상 DB (기본 roadnet)")
    ap.add_argument("--sql", default=str(DEFAULT_SQL), help="실행할 SQL 파일")
    ap.add_argument("--super-user", default="postgres", help="슈퍼유저 (기본 postgres)")
    ap.add_argument("--super-password", default=os.environ.get("PGPASSWORD"), help="슈퍼유저 비밀번호")
    ap.add_argument("--host", default="", help="DB host (미지정 시 sudo peer 인증)")
    ap.add_argument("--port", type=int, default=5432, help="DB port")
    ap.add_argument("--app-user", default="mytobcom", help="시뮬레이터 접속 계정")
    ap.add_argument("--app-password", default="my664761", help="시뮬레이터 접속 비밀번호")
    ap.add_argument("--check", action="store_true", help="생성 후 mytobcom 접속/권한 점검")
    ap.add_argument("--check-only", action="store_true", help="생성 없이 점검만")
    args = ap.parse_args()

    if not args.check_only:
        sql_path = Path(args.sql)
        if not sql_path.exists():
            sys.exit(f"[setup] SQL 파일 없음: {sql_path}")
        print(f"[setup] {args.dbname} DB 에 스키마/테이블 생성 중 ... ({sql_path.name})")
        rc = run_psql_super(args, ["-f", str(sql_path)])
        if rc != 0:
            sys.exit(f"[setup] 스키마 생성 실패 (psql rc={rc}). roadnet DB 존재 여부와 슈퍼유저 권한을 확인하세요.")
        print("[setup] 스키마/테이블 생성 완료.")

    if args.check or args.check_only:
        rc = check_as_mytobcom(args)
        if rc != 0:
            sys.exit(f"[check] 점검 실패 (psql rc={rc}).")
        print("[check] 점검 완료.")


if __name__ == "__main__":
    main()
