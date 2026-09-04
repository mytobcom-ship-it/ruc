/**
 * @file SingleInstanceLock.h
 * @brief 중복 기동 방지(단일 인스턴스 보장) 헤더 — 실행 디렉터리의 PID 파일에 배타적
 *   flock(파일 잠금)을 걸어 이미 살아있는 인스턴스가 있으면 새 기동 시도를 즉시 실패시킨다.
 * @remark 구현은 AppMain.cpp 에 있다(잠금 fd 는 프로세스 생명 주기 내내 열어둬야 하는 상태값이라
 *   main() 과 같은 번역 단위에 두는 게 자연스러움). 이 헤더는 Server.cpp 의 1초 주기 타이머에서
 *   잠금 무결성(IsSingleInstanceLockIntact)을 주기적으로 재확인하기 위해 분리했다 — PID 파일이
 *   기동 중 실수로 삭제(rm 등)되면 flock 은 이미 열어둔 fd(구 inode)에만 유효하고, 그 경로에
 *   새로 생성되는 파일은 다른 inode 라 새 프로세스가 거기 새로 잠금을 걸어버려 중복 기동 방지가
 *   뚫리는 것을 실측으로 확인(2026-09-04)했다 — 그 구멍을 좁히기 위한 주기적 자가점검용
 *   (2026-09-04 최정우 추가, 사용자 지시)
*/
#ifndef __SINGLE_INSTANCE_LOCK_H__
#define __SINGLE_INSTANCE_LOCK_H__

/**
 * @brief 중복 기동 방지 — 실행 디렉터리의 PID 파일에 배타적 flock 을 시도한다. main() 진입
 *   직후, config.ini 조차 읽기 전에 가장 먼저 호출해야 무거운 초기화를 시작하기 전에 빠르게
 *   실패한다.
 * @return true(잠금 획득 성공, 유일한 인스턴스), false(이미 다른 인스턴스가 실행 중이거나 오류)
*/
bool AcquireSingleInstanceLock();

/**
 * @brief 잠금이 걸려있는 PID 파일이 여전히 그 경로에 그대로 존재하는지(삭제·교체되지 않았는지)
 *   확인한다. 어긋나 있으면(외부에서 rm 등으로 삭제된 뒤일 가능성) 같은 경로에 파일을 즉시
 *   다시 만들어 새 fd 로 재점유해 구멍을 최대한 좁힌다 — 원래 잠금 자체는 잃지 않으므로(별도
 *   fd), 이 재점유는 "혹시 몰라 경로를 다시 막아두는" 보강일 뿐이다. 1초 주기 타이머 등에서
 *   반복 호출하는 용도.
 * @return true(정상), false(경로가 사라져 있었음 — 재점유 시도함, 호출측에서 경고 로그 권장)
*/
bool IsSingleInstanceLockIntact();

#endif
