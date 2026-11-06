# 게임 안 시험 절차 (1차, 2026-11-06)

시험판: DLL `3482f0e14a3303b317b7bfb2008b7d28`, ESP `ec206b62fd7e096929974a8e66f4e1ca`, INI `e1e400c13036246f4c4692794ed34c35` (커밋 2e0ad4a).
판정은 로그로 Claude가 한다: `Documents\My Games\Skyrim Special Edition\SKSE\SCSSoulCharge.log`의 `SELFTEST PASS/FAIL/SKIP` 줄, `SpellCastingStaff_1.6.1170.log`(시험 동안 `bVerboseLog=1`).

## 준비 (Claude, 게임·MO2를 끈 뒤)
- MO2에 새 모드 `SCSSoulCharge` 추가(`##Magic`, `Spellcasting Staff NG` 바로 아래), 플러그인 `SCSSoulCharge.esp`를 `SpellCastingStaff.esp` 바로 뒤에 켬.
- SCS INI `bVerboseLog=1`(시험 뒤 0으로 되돌림, 원본은 Mod-Management `backups/`에 보관).
- 시험판 설정: 변환 대상은 "지팡이 - 화염 화살"(Skyrim.esm 4C6CA) 하나뿐.

## 사용자 할 일 (버리는 시험용 세이브에서)
변환한 아이템과 코세이브 데이터가 세이브에 남으므로 아끼는 세이브로 하지 않는다.

1. MO2에서 게임 실행, 아무 세이브나 불러온 뒤 바로 새 이름으로 저장해 시험용 세이브를 만든다.
2. 콘솔(`~`)에 차례로 입력:
   - `player.additem 0004C6CA 1` (화염 화살 지팡이)
   - `help SCSSC 4` → 줄 중 `SCSSC_TestStaffFireball`의 ID를 확인 → `player.additem <그 ID> 1` (시험용 화염구 지팡이)
3. 화염 화살 지팡이를 오른손에 든다.
4. 마법 메뉴를 열면 이름이 "화염 화살"인 주문이 새로 생겨 있다(매지카 비용 0). 그것을 **오른손**에 지정한다. 이미 같은 이름의 진짜 주문을 알고 있으면 비용이 0인 쪽을 고른다.
5. 무기를 꺼낸 상태에서 공격 버튼으로 세 발 쏜다. 불덩이가 나가는지 본다.
6. 지팡이를 해제했다가 다시 든다.
7. 충전이 바닥날 때까지 쏜다. "Staff charge too low"가 뜨고 더 안 나가는지 본다.
8. 시험용 화염구 지팡이로 바꿔 들고 한 발 쏜다(이 지팡이는 일부러 변환하지 않은 바닐라 지팡이).
9. 저장 → 메인 메뉴로 나가기 → 그 세이브를 다시 불러오기.
10. 게임을 끈다. 끌 때 튕기면 알려 준다.

눈으로 봐 줄 것(로그로 못 보는 것): 시전 시간이 바닐라 지팡이와 비슷한지, 지팡이 이름이 주문 이름으로 깜빡이는지, 충전 막대 모양.

## 판정표 (Claude)
| 줄 | 뜻 | FAIL일 때 할 일 |
|---|---|---|
| T-ORD | 키워드가 SCS 지팡이 목록보다 먼저 붙음. SCS 로그 "ready: N keyword staffs"의 N = T-ORD의 weaponsWithScsKeyword | `skippedBeforeReady > 0`이면 폼이 플러그인별로 초기화됨 → 설계 1항 (라) 생성기 방식 |
| T-ESP, T-VMAD | ESP 로드, 스크립트 지팡이 판정 | T-VMAD FAIL이면 스크립트 판정을 다른 방법으로 |
| T-C2a, T-C2b | 마법부여가 아이템과 손에 걸림 | T-C2b FAIL(손이 원래 ENCH로 시전) → 마법부여를 붙인 뒤 같은 아이템을 다시 장착하는 처리 추가 |
| T-C1 | 발사 허락·차감·거부 | 차감이 안 되면 액터 값 쓰기 방식 재검토 |
| T-C3 | 해제 뒤 ExtraCharge = 손에 있던 충전 | `extraCharge=none`인데 충전이 줄어 있었으면 해제 때 ExtraCharge를 직접 씀 |
| T-C4 | 바닐라 한 발 소모 = CalculateMagickaCost(player 또는 nullptr) | 맞은 쪽으로 `ChargeCost` 고정, 둘 다 아니면 비율 기록 |
| T-C5 | 공유 ENCH 시전 때 엔진이 충전을 따로 안 뺌 | 엔진 차감분만큼 보정 |
| T-NAME, T-BARE, T-SAVE | 이름 지키기, 맨손 차단, 세이브 | 각 줄의 내용대로 |
| T-EXIT | 게임 종료 때 튕김 없음(크래시 로그 없음) | 효과를 독립 복사하는 방식으로 변경 |
