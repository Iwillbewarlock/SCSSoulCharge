# SCSSoulCharge 설계 (2026-11-06, 0단계 확인 반영본)

Spellcasting Staff NG(이하 SCS, Nexus 196207, 작성자 Hamkidna, MIT)의 애드온.
작성자 허락 받음(2026-11-06). 작성자는 당분간 SCS를 업데이트하지 않음 → SCS 2.1.1 고정 대상, API 요청은 기다리지 않음.
쌍지팡이 지원은 하지 않음(사용자 결정).
0단계 확인 근거는 docs/phase0-findings.md.

## 목표
바닐라·다른 모드 지팡이를 SCS 촉매로 쓰게 하고, 지팡이에 원래 박힌 마법은 "그 지팡이의 소울젬 충전을 쓰는 주문"으로 바꿔 마법 탭에서 고르게 한다.
모드 지팡이도 지원한다(사용자 지시 2026-11-06). 이 모드팩 기준 대상: 마법부여 있는 지팡이 2289개(정의 플러그인 75개), 서로 다른 지팡이 ENCH 761개.

## 대상
SE 1.5.97, AE 1.6.1170, AE 1.7.104 (SCS와 같음). VR 제외. CommonLibSSE-NG.
처음에는 SCS 기본(Basic) 종류 키워드만 붙인다(DualCast는 비용 0 주문에도 기본 비용이 붙을 수 있음: reference/scs-2.1.1/include/SpellCastingStaff/DualCastCost.h:39-49).
StaffCasting(D:\DEV\StaffCasting)과는 함께 쓰지 않는다(장착 처리·지팡이 주문 생성이 충돌). DLL은 StaffCasting.dll을 발견하면 경고만 남긴다.

## SCS 2.1.1에서 확인한 사실 (reference/scs-2.1.1 기준 줄 번호)
- 인식: base WEAP에 `SpellCastingStaff_Keyword`(0x803) + 27개 종류 키워드 중 정확히 하나(StaffProfile.h, Equipment.cpp:261-278). 종류 키워드가 잘못돼도 IsStaff는 참이라 "지팡이 하나" 규칙에는 참여.
- 스캔: kDataLoaded에 한 번, 모든 WEAP(Plugin.cpp:103-104, Controller.cpp:68-121).
- 공유 ENCH 0x80B(cost 0, 수동 비용, Staff Enchantment). 시전 판정 다수가 손 캐스터 currentSpell == 공유 ENCH(Casting.cpp:157,207,664; Charge.cpp:252).
- 진짜 주문은 SCS가 SpellFire 순간 kOther 캐스터에서 `CastSpellImmediate`로 직접 쏜다: F&F Casting.cpp:490(:456, :480 소환무기), 집중형 :782, Rapid :698.
- 비용 경로: 일반 F&F는 escrow(Charge.cpp:153-187), Rapid는 매 발 직접(Casting.cpp:675-698), 집중형 매지카는 엔진이 currentSpellCost로 차감(Casting.cpp:775-777). 비용은 `CalculateMagickaCost`(Charge.cpp:151).
- 주문 유효: player->HasSpell, kSpell, F&F 또는 Concentration(Equipment.cpp:293-304).
- 주문 기억: base WEAP FormID → SPEL FormID, 코세이브(Serialization.cpp), HasSpell 실패 시 기억 유지·재시도(Equipment.cpp:157-172).
- 지팡이를 든 손에서 메뉴로 주문을 고르면 SCS가 장착을 가로챔("equip consumed", Equipment.cpp:79). 지팡이 없는 손이면 엔진에 넘김(Equipment.cpp:38-42).
- 지팡이 두 개면 하나를 강제 해제(Equipment.cpp:95-130).
- SCS는 고른 주문 이름으로 base WEAP 이름을 바꾼다(Equipment.cpp:59-60). 같은 종류 지팡이 전부의 이름이 바뀌어 보인다(화면만 영향).
- SCS는 플레이어 전용. ActorMagicCaster 가상표 훅은 슬롯 0x1C 하나뿐(CasterDiagnostics.cpp:404-415).

## 엔진에서 확인한 사실
- 바닐라 지팡이는 장착 중 손 충전 액터 값(`kRightItemCharge` 64 / `kLeftItemCharge` 82)이 충전 중에 `currentSpellCost`만큼 줄고, ExtraCharge는 해제 때 맞춰진다(StaffCasting-spike.log:17-37, :262).

## 설계
1. 키워드: 로드 오더의 지팡이(SCS 지팡이 제외)에 키워드 두 개(0x803 + Basic 단계 하나)를 붙인다. base EITM은 건드리지 않는다(NPC 지팡이 보호).
   - 방식: 배포용 DLL 방식(사용자 결정 2026-11-06, 원문 "배포용 DLL 방식"). 애드온 DLL이 무기 폼 초기화(`TESObjectWEAP::InitItemImpl` 가상표 슬롯 0x13 훅, kDataLoaded보다 앞) 직후 조건에 맞는 지팡이에 `BGSKeywordForm::AddKeyword`(CommonLib BGSKeywordForm.h:29)로 키워드를 붙인다. 준비(두 ESP 로드, 런타임 확인, 필수 훅 설치) 중 하나라도 실패하면 하나도 안 붙인다. ESP 키워드 패치·생성기 스크립트는 만들지 않는다(마스터 75개 ESP는 배포 불가).
   - 순서 조건: SCS는 kDataLoaded에 지팡이 목록을 한 번만 만들고 다시 만들지 않는다(Controller.cpp:69 `if (enabled_) return true;`, :112-121). 우리 부여가 그보다 먼저 끝나야 한다. → 자가 테스트 T-ORD: 우리 DLL이 부여 끝 시각과 붙인 개수를 로그에 남기고, SCS 로그 "SpellCastingStaff ready: N keyword staffs"(Controller.cpp:182)의 N이 27 + 우리 개수인지 비교.
   - 순서 가설과 대안:
     (가) 채택: 무기 폼 초기화(InitItemImpl) 훅에서 붙인다. 폼 초기화는 데이터 로드 과정 안에서 끝나고 kDataLoaded 알림은 그 뒤라서, 어느 DLL이 먼저 알림을 받든 SCS 목록 작성보다 앞선다(가설, T-ORD로 확인).
     (나) 버림: kDataLoaded에서 DLL 이름순에 기대는 방법. SKSE는 폴더 열거 순서대로 등록하며 이름 정렬 보장이 없다(GPT 검토, skse64 PluginManager.cpp). MO2 가상 폴더에서는 더 불확실하다.
     (다) 버림: kInputLoaded. 폼 로드 완료 보장이 없다.
     (라) (가)가 실패하면: 생성기 ESP를 사용자 PC에서 만드는 도구(배포는 도구만)로 되돌린다.
   - 거를 것: 마법부여 없음, 비용 0 ENCH, 이미 SCS 키워드 있음, Staff 애니메이션인데 다른 무기로 보이는 것(로그로 목록 보고), 스크립트 붙은 지팡이(VMAD 있음, 이 모드팩 7개: 기본 제외, 설정으로 켜기). 제외 목록·포함 목록은 INI로 받는다.
   - Basic 단계(01~05)를 고르는 규칙은 설정으로 둔다(예: EAMT·가치 기준).
2. 마법부여 교체: 플레이어가 장착하는 지팡이 아이템에만 ExtraEnchantment로 공유 ENCH를 붙인다(최대 충전 = base EAMT). 버리기·거래·동료에게 줄 때 되돌린다. 같은 base의 모든 아이템이 SCS 지팡이로 인식되므로 장착 즉시 변환해야 한다.
3. 충전 주문: ESP의 빈 SPEL 자리 1024개를 런타임에 채운다(효과·조건 독립 복사, kSpell, 수동 비용(NoAutoCalc) 플래그 + 매지카 비용 0, 시전 시간 = 원래 ENCH ChargeTime, 장착 칸 설정, MGEF는 수정 금지). 플레이어가 실제로 든 ENCH에만 자리를 준다. ENCH↔자리 표는 코세이브에 저장하고, 로드 때 저장된 표를 먼저 적용한 뒤 새 ENCH만 빈자리에 넣는다. 한 세이브 안에서 자리는 재사용하지 않는다(SCS가 지팡이→SPEL을 자기 코세이브에 기억하므로, Equipment.cpp:57-58). 1024개가 차면 새 변환을 거부하고 알린다. 효과는 원래 ENCH의 `Effect*`를 공유하고, `hostileCount`·`avEffectSetting`도 옮긴다. 코세이브 상태 전이는 docs/plan-phase4.md "GPT 설계 검토 반영" 5번.
4. 지급: 변환 지팡이를 들면 AddSpell, 내려놓으면 RemoveSpell. 지급 뒤 SCS 재동기화(장비 이벤트)를 보장.
5. 비용(SCS 수정 없음). 한 지점: `ActorMagicCaster` 가상표 슬롯 0x01 `CastSpellImmediate` 훅(가상표 ID SE 257613 / AE 205828). 플레이어 캐스터 + 풀 SPEL일 때만 처리.
   - F&F·Rapid: 충전 ≥ 비용이면 원래 함수 호출 뒤 비용을 뺀다. 단, TargetLocation 실패(돌아온 뒤 kOther가 `currentSpell == spell && state == 6`, Casting.cpp:521-524)면 빼지 않는다. 충전 부족이면 원래 함수를 부르지 않고 "충전 부족" 알림(효과 없음, SCS 발사 소리는 남음).
   - 집중형: 비용은 초당 값이다. 충전 > 0이면 시작을 허락하고(시작 때 한 번에 빼지 않음), 매 프레임 kOther가 그 SPEL을 시전 중이면 비용×dt를 뺀다(dt가 0.25초를 넘으면 끊음). 바닥나면 `MagicCaster::InterruptCast`(SE 33630 / AE 34408). 프레임 훅은 SCS와 같은 `Main::Update`(SE 35565 / AE 36564) call 지점에 사슬로 잇는다(FrameHook.cpp:39-63).
   - 충전을 빼는 곳: 장착 중인 지팡이 손의 충전 액터 값, `ModActorValue(kDamage, k(Left|Right)ItemCharge, -양)`. ExtraCharge를 직접 깎지 않는다.
   - 맨손 차단: 풀 SPEL을 실제로 손에 장착하려는 시도를 거부(그대로 유지. 손에 든 주문은 이 훅을 지나지 않음).
   - 비용 = 원래 ENCH의 런타임 계산값(`CalculateMagickaCost`, SE 11213 / AE 11321). 레코드 숫자는 쓰지 않는다(자동 계산 ENCH 211개). player/nullptr 중 어느 인자가 바닐라와 같은지는 T-C4로 정한다.
   - 선택(설정, 기본 꺼짐): 충전이 모자라면 풀 SPEL을 잠시 RemoveSpell해 SCS가 버튼 누를 때 거절하게 한다. 그동안 마법 탭에서 주문이 사라진다.
6. 직접 마법부여한 지팡이(ExtraEnchantment가 이미 있는 아이템)는 기본 제외.
7. 밸런스: 효과를 공유하므로 위력 배율은 이번 범위에서 뺀다(공유 효과를 바꾸면 원래 지팡이도 바뀜). SCS 위력 특기·지팡이 버프, 플레이어 학파 특기가 붙는 정도는 T-C6로 재고, 필요하면 나중에 효과를 따로 복사하는 방식으로 배율을 넣는다.
8. 이름 지키기(설정, 기본 켜짐, 사용자 요청 2026-11-06): SCS는 고른 주문 이름을 base WEAP 이름에 쓴다(Naming.cpp:7-15, 메뉴 미리보기 InventoryNamePreview.cpp:93-100). SCS는 쓴 이름을 `appliedNames_`에 기억하고 원하는 이름이 바뀔 때만 다시 쓴다(Naming.cpp:11). 그래서 우리가 변환한 지팡이 base의 이름이 kDataLoaded 때 저장한 원래 이름과 다르면 원래 이름으로 되돌린다. 되돌린 뒤에는 SCS가 다음 주문 변경 때까지 다시 쓰지 않는다.
   - 확인 지점: 우리 프레임 훅(일시정지 메뉴 중에도 돌아야 함. SCS 미리보기는 메뉴 중에 돈다 FrameHook.cpp:18-24) + 장비 이벤트. 비교 대상은 변환한 base만이라 비용이 작다.
   - SCS 자체 지팡이 27개는 건드리지 않는다(SCS 고유 기능).
   - SCS는 플레이어 소지품 중 그 base 항목에 이미 있는 ExtraTextDisplayData(개별 이름)도 덮어쓴다(Naming.cpp:20-38). 변환 대상은 개별 이름이 없는 것이 보통이지만, 있으면 그 값도 원래대로 되돌린다.
   - 가설: SCS와 우리 프레임 훅 순서에 따라 메뉴에서 한 프레임 동안 주문 이름이 보일 수 있다. 자가 테스트 T-NAME: 주문을 고른 뒤 다음 프레임에 base 이름 = 원래 이름이면 PASS. 깜빡임은 눈으로만 판단 가능해서 사용자 확인 항목.

## 0단계 확인 목록 (결과: docs/phase0-findings.md)
C1~C7 정리 완료. 게임을 켜야 아는 항목은 자가 테스트 T-C1~T-C6, T-ORD로 확인한다.

## 단계
0 사실 확인(완료) → 2 ESP(풀 SPEL 자리, 시험용 지팡이) → 3 (키워드 생성기는 DLL 방식으로 바뀌어 4단계에 합침) → 4 DLL → 5 플러그인 안 자가 테스트(PASS/FAIL 로그) → 6 reviewer 에이전트 + GPT 리뷰 → 사용자 인게임 확인(화면·손맛만).

## 참고
- reference/scs-2.1.1: SCS 2.1.1 소스(MIT, 저작권 Hamkidna). 수정하지 않는다.
- Immersive Staves Tweaks(196064, GPL-3) 소스의 충전 고갈 시 매지카 차감 방식(MasterNativeCostHooks.cpp:434-475)은 방식만 참고, 코드 복사 금지.
- GPT 설계 검토(2026-11-06): 지적 11건 모두 소스와 일치, 위 설계에 반영.
