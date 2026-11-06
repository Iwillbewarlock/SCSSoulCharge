# SCSSoulCharge 설계 (2026-11-06)

Spellcasting Staff NG(이하 SCS, Nexus 196207, 작성자 Hamkidna, MIT)의 애드온.
작성자 허락 받음(2026-11-06). 작성자는 당분간 SCS를 업데이트하지 않음 → SCS 2.1.1 고정 대상, API 요청은 기다리지 않음.
쌍지팡이 지원은 하지 않음(사용자 결정).

## 목표
바닐라·다른 모드 지팡이를 SCS 촉매로 쓰게 하고, 지팡이에 원래 박힌 마법은 "그 지팡이의 소울젬 충전을 쓰는 주문"으로 바꿔 마법 탭에서 고르게 한다.

## 대상
SE 1.5.97, AE 1.6.1170, AE 1.7.104 (SCS와 같음). VR 제외. CommonLibSSE-NG.
처음에는 SCS 기본(Basic) 종류 키워드만 붙인다(DualCast는 비용 0 주문에도 기본 비용이 붙을 수 있음: reference/scs-2.1.1/include/SpellCastingStaff/DualCastCost.h:39-49).

## SCS 2.1.1에서 확인한 사실 (reference/scs-2.1.1 기준 줄 번호)
- 인식: base WEAP에 `SpellCastingStaff_Keyword`(0x803) + 27개 종류 키워드 중 정확히 하나(StaffProfile.h, Equipment.cpp:261-278). 종류 키워드가 잘못돼도 IsStaff는 참이라 "지팡이 하나" 규칙에는 참여.
- 스캔: kDataLoaded에 한 번, 모든 WEAP(Plugin.cpp:103-104, Controller.cpp:68-121).
- 공유 ENCH 0x80B. 시전 판정 다수가 손 캐스터 currentSpell == 공유 ENCH(Casting.cpp:157,207,664; Charge.cpp:252). 집중형은 kOther로 별도 경로(Casting.cpp:735-785).
- 비용 경로 3가지: 일반 F&F는 escrow(Charge.cpp:153-187), Rapid는 매 발 직접(Casting.cpp:675-698), 집중형 매지카는 엔진이 currentSpellCost로 차감(Casting.cpp:775-777).
- 주문 유효: player->HasSpell, kSpell, F&F 또는 Concentration(Equipment.cpp:293-304).
- 주문 기억: base WEAP FormID → SPEL FormID, 코세이브(Serialization.cpp), HasSpell 실패 시 기억 유지·재시도(Equipment.cpp:157-172).
- 지팡이를 든 손에서 메뉴로 주문을 고르면 SCS가 장착을 가로챔("equip consumed", Equipment.cpp:79). 지팡이 없는 손이면 엔진에 넘김(Equipment.cpp:38-42).
- 지팡이 두 개면 하나를 강제 해제(Equipment.cpp:95-130).
- SCS는 플레이어 전용.

## 설계
1. 키워드: 생성기 스크립트가 로드 오더의 지팡이(SCS 지팡이 제외)에 키워드 두 개를 붙인 ESP 패치를 만든다. base EITM은 건드리지 않는다(NPC 지팡이 보호).
2. 마법부여 교체: 플레이어가 장착하는 지팡이 아이템에만 ExtraEnchantment로 공유 ENCH를 붙인다. 버리기·거래·동료에게 줄 때 되돌린다. 같은 base의 모든 아이템이 SCS 지팡이로 인식되므로 장착 즉시 변환해야 한다.
3. 충전 주문: ESP의 빈 SPEL 자리 256개를 런타임에 채운다(효과·조건 독립 복사, kSpell, 매지카 비용 0, 시전 시간·장착 칸 설정, MGEF는 수정 금지). ENCH↔자리 표는 코세이브에 저장하고, 로드 때 저장된 표를 먼저 적용한 뒤 새 ENCH만 빈자리에 넣는다.
4. 지급: 변환 지팡이를 들면 AddSpell, 내려놓으면 RemoveSpell. 지급 뒤 SCS 재동기화(장비 이벤트)를 보장.
5. 비용(SCS 수정 없음): 
   - F&F: DLL이 매 프레임 "든 지팡이 충전 ≥ 비용"을 전역 변수에 기록, 주문 효과 조건으로 사용. 발사 확인 시 충전 차감.
   - 집중형: 시전 중 매 프레임 비용×dt 차감, 바닥나면 InterruptCast.
   - 맨손 차단: 풀 SPEL을 실제로 손에 장착하려는 시도를 거부.
   - 비용 = 원래 ENCH 비용(바닐라 실제 소모량과 같은지 0단계 확인).
6. 직접 마법부여한 지팡이(ExtraEnchantment가 이미 있는 아이템)는 기본 제외.

## 0단계 확인 목록
C1. 시전을 막고 충전을 뺄 정확한 위치(엔진 함수만, SCS 내부 비의존). F&F 발사 확인 신호.
C2. ExtraEnchantment로 붙인 공유 ENCH가 SCS 판정을 통과하는지.
C3. ExtraCharge: 없으면 최대 충전 취급, 변환 후 유지, 재충전 화면 작동.
C4. 원래 ENCH 비용 = 바닐라 1회 소모량인지(마법부여 기술·비용 감소 효과 영향).
C5. 공유 ENCH 네이티브 시전이 충전을 깎는지.
C6. 파괴 특기 등이 충전 주문에 붙는 밸런스 → 설정 옵션.
C7. D:\DEV\StaffCasting(0.1.0)과 겹치는지.

## 단계
0 사실 확인 → 2 ESP → 3 키워드 패치 생성기 → 4 DLL → 5 플러그인 안 자가 테스트(PASS/FAIL 로그) → 6 reviewer 에이전트 + GPT 리뷰 → 사용자 인게임 확인(화면·손맛만).

## 참고
- reference/scs-2.1.1: SCS 2.1.1 소스(MIT, 저작권 Hamkidna). 수정하지 않는다.
- Immersive Staves Tweaks(196064, GPL-3) 소스의 충전 고갈 시 매지카 차감 방식(MasterNativeCostHooks.cpp:434-475)은 방식만 참고, 코드 복사 금지.
- GPT 설계 검토(2026-11-06): 지적 11건 모두 소스와 일치, 위 설계에 반영.
