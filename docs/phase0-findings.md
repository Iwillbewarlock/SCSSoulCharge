# 0단계 확인 결과 (2026-11-06)

근거 표기:
- `scs:` = `reference/scs-2.1.1/` (SCS 2.1.1 소스)
- `cl:` = CommonLibSSE-NG 10.1.0 (`C:/DEV/CausticLight/plugin/extern/CommonLibSSE-NG`, 커밋 39f9d07)
- `ist:` = Immersive Staves Tweaks 소스 1.5.97판 (`D:/TAKEALOOK/downloads/Immersive Staves Tweaks -Source 196064 v1 ...zip`, 압축 안 `Source/ImmersiveStavesTweaks-source-1.5.97/src/`). 방식 참고만, 코드 복사 금지.
- `spike-log:` = `C:/Users/qtuna/Documents/My Games/Skyrim Special Edition/SKSE/StaffCasting-spike.log` (2026-11-02, AE 1.6.1170, 바닐라 "지팡이 - 화염 화살" WEAP 0004C6CA / ENCH 0004C6CB 실제 시전 기록)
- 레코드: houseCARL, TAKEALOOK 프로필 'TKL - MUNG ADDON', epoch e2-90853e410e1e476a. SCS ESP는 모드팩에 설치돼 있지 않아서, 받아 둔 `Spellcasting Staff Korean ver-196207-2-1-1` 압축을 scratchpad에 풀어 직접 읽었다(읽기 스크립트 scratchpad `tools/espdump.py`).

## 먼저 알아야 할 두 가지

1. SCS는 마법을 지팡이 마법부여로 쏘지 않는다. 손에 걸린 공유 ENCH(0x80B)는 애니메이션용 가짜이고, `SpellFire` 순간 SCS가 진짜 SPEL을 `MagicCaster::CastSpellImmediate`로 kOther 캐스터에서 직접 쏜다.
   - F&F: scs:src/Casting.cpp:490 (일반), :456 (왼손 소환무기), :480 (소환무기 손 지정)
   - 집중형: scs:src/Casting.cpp:782. 직전에 `currentSpellCost = cost`(:777)를 넣고, 이후 매지카 차감은 엔진이 한다.
   - Rapid(Swift): 0.5초마다 같은 함수 재호출(scs:src/Casting.cpp:698)
2. 바닐라 지팡이 충전은 ExtraCharge를 바로 깎지 않는다. 장착 중에는 액터 값 `kRightItemCharge`(64) / `kLeftItemCharge`(82)(cl:include/RE/A/ActorValues.h:72, :90)가 깎이고, ExtraCharge는 해제한 뒤에 맞춰진다.
   - spike-log:17-37: 화염 화살 1발. 매지카 20.0 그대로, 충전 액터 값 677.01 → 675.09(캐스터 상태 1→2) → 640.52(2→3). 깎인 합 36.49 = 그때 `currentSpellCost` 36.5. 깎이는 때는 발사가 아니라 충전 중이다. ExtraCharge 원값은 677.01 그대로.
   - spike-log:262: 해제·재장착 뒤 원값 604.03 = 677.01 − 36.49×2. 해제 때 액터 값이 ExtraCharge로 되쓰인다는 정황.
   - 그래서 우리도 장착 중에는 손 충전 액터 값을 깎아야 한다. ExtraCharge를 직접 깎으면 해제 때 엔진 값으로 덮일 수 있다.

## 항목별 결과

### C1. 시전을 막고 충전을 뺄 위치, F&F 발사 신호

확인됨:
- 위치: `ActorMagicCaster` 가상표 슬롯 0x01 `CastSpellImmediate`. 가상 함수 선언 cl:include/RE/M/MagicCaster.h:62, 가상표 ID SE 257613 / AE 205828 (cl:include/RE/Offsets_VTABLE.h:1862). SCS 내부에 기대지 않는 엔진 함수다. SCS의 호출은 가상 호출이라 이 슬롯을 지난다.
- SCS는 이 가상표의 슬롯 0x1C 하나만 건다(scs:src/CasterDiagnostics.cpp:404-415, 주석 "This is the ONLY ActorMagicCaster hook"). 슬롯 0x01은 비어 있다.
- F&F 발사 신호 = 풀 SPEL로 이 함수가 불린 한 번. SCS가 이미 "한 번 누름에 한 발" 걸러서 부른다(scs:src/Casting.cpp:164-172).
- TargetLocation(룬·소환 위치) 실패: 함수가 돌아온 뒤 kOther가 `currentSpell == spell && state == 6`이면 실패로 보고 SCS가 되돌린다(scs:src/Casting.cpp:521-557). 우리도 같은 조건이면 충전을 빼지 않는다.
- 집중형 중단: `MagicCaster::InterruptCast(bool)` SE 33630 / AE 34408 (cl:src/RE/M/MagicCaster.cpp:37-42). SCS도 같은 함수를 쓴다(scs:src/Casting.cpp:53-58).
- 집중형 진행 판정: kOther `currentSpell == 풀 SPEL && state != 0` (SCS와 같은 판정 scs:src/Casting.cpp:785, :895-898).
- 우리가 kOther를 중단하면 SCS는 다음 프레임에 "caster finished"로 보고 채널 기록만 정리하고, 이미 쉬는 캐스터는 다시 건드리지 않는다(scs:src/Casting.cpp:893-898, :840-842). 충돌 없음.
- 매 프레임 차감에 쓸 프레임 훅: SCS와 같은 `Main::Update` SE 35565 / AE 36564 안 call 지점에 `write_call<5>`로 잇는다(scs:src/FrameHook.cpp:39-63, 지점 오프셋은 scs의 `RuntimeCompatibility::FrameCallOffset()`). SCS도 E8 call을 사슬로 잇는 방식이라 같이 걸 수 있다.

가설(확인 방법):
- 훅에서 원래 함수를 부르지 않으면 효과가 안 나간다. SCS는 상태 0을 보고 "watchdog not needed"로 끝내고 발사 소리만 낸다(scs:src/Casting.cpp:572-581). → 자가 테스트 T-C1.

### C2. ExtraEnchantment로 붙인 공유 ENCH가 SCS 판정을 통과하는지

확인됨:
- SCS의 지팡이 판정은 base WEAP 키워드만 본다(scs:src/Equipment.cpp:261-278). 마법부여는 안 본다.
- 메뉴 가로채기는 base WEAP가 `staffs_` 목록에 있어야 한다(scs:src/Equipment.cpp:37). 목록은 kDataLoaded에 키워드가 있는 모든 WEAP로 만든다(scs:src/Controller.cpp:112-121). ESP로 키워드를 붙이면 들어간다.
- 시전 시작·충전 진행 판정은 손 캐스터 `currentSpell == 공유 ENCH`다(scs:src/Charge.cpp:252-253, scs:src/Casting.cpp:157).

가설(확인 방법):
- 엔진이 손 캐스터에 넣는 ENCH가 ExtraEnchantment 쪽인지. 정황: CommonLib의 재구현 `TESObjectREFR::GetEnchantment`가 ExtraEnchantment를 먼저 본다(cl:src/RE/T/TESObjectREFR.cpp:337-350). 엔진 본체 `InventoryEntryData::GetEnchantment`는 SE 15788 / AE 16026(cl:src/RE/I/InventoryEntryData.cpp:105-110)인데 안쪽 순서는 미확인. → T-C2a, T-C2b.

### C3. ExtraCharge

확인됨:
- 장착 중 충전의 실제 값은 손 충전 액터 값이고 ExtraCharge 원값은 해제 때 맞춰진다(위 "두 가지" 2번, spike-log:17-37, :262).
- ExtraEnchantment 구조: `enchantment`, `charge`(uint16, 최대 충전), `removeOnUnequip` (cl:include/RE/E/ExtraEnchantment.h:23-27). ExtraCharge는 float 하나(cl:include/RE/E/ExtraCharge.h:24).
- 모드팩 지팡이 2289개 모두 EAMT가 있다(500~5000, 0은 없음).

가설(확인 방법):
- ExtraCharge가 없으면 가득 찬 것으로 본다. 정황 두 개: CommonLib 퍼센트 공식(cl:src/RE/I/InventoryEntryData.cpp:112-140, ExtraCharge 없음 → 100%), spike-log:302 "noExtraCharge(full?) max=750 av=750.00". 다만 그 로그 구간은 지팡이가 두 개였을 수 있어 확정 못 함.
- 변환 때 ExtraEnchantment.charge를 base EAMT로 넣으면 손 충전 액터 값 최대치·충전 막대·소울젬 재충전 화면이 그대로 작동한다. → T-C3, 재충전은 사용자 1회.

### C4. 원래 ENCH 비용 = 바닐라 1회 소모량인지

확인됨:
- 바닐라 1발 소모량 = 그 시전의 `currentSpellCost`(spike-log:18, :28. 36.49 vs 36.5, 표본 1개, 화염 화살).
- 모드팩 지팡이 ENCH 761개 중 자동 계산이 211개다(예: StaffEnchFireball, 승자 Synthesis - Sorcerer.esp, Flags=0, 기록된 비용 60). 자동 계산 ENCH는 레코드 숫자가 아니라 엔진 계산값을 쓴다. 엔진 계산 함수 `MagicItem::CalculateCost` SE 11213 / AE 11321(cl:src/RE/M/MagicItem.cpp:42-47). IST도 같은 AE ID를 비용 계산 지점으로 쓴다(ist:MasterNativeCostHooks.cpp:497, AE ID 11321).
- 비용 0인 ENCH 4개.

가설(확인 방법):
- `origEnch->CalculateMagickaCost(player)` = 바닐라 `currentSpellCost`인지, 마법부여 기술·특기 영향이 있는지. → T-C4. ESP에 SCS 키워드 없는 시험용 지팡이(바닐라 화염구 지팡이 복사본)를 두고, 사용자가 한 발 쏘면 플러그인이 손 캐스터 `currentSpellCost`, 충전 액터 값 차이, `CalculateMagickaCost(player)`, `CalculateMagickaCost(nullptr)`, ENIT 숫자를 한 줄로 남긴다. 지팡이 공격 입력을 플러그인이 대신 만들 방법은 못 찾아서 사용자 동작 1회가 필요하다.

### C5. 공유 ENCH 네이티브 시전이 충전을 깎는지

확인됨:
- 공유 ENCH 0x80B: cost=0, Flags=0x1(NoAutoCalc, 수동 비용), castType=2(집중형, 실행 중 SCS가 바꿈 scs:src/Equipment.cpp:200-258), enchantType=0xC(Staff Enchantment), amount=0. SCS 지팡이 27개 모두 EITM=0x80B, EAMT=0.
- SCS 코드에는 ExtraCharge·충전 액터 값을 다루는 곳이 없다(전체 검색 0건).
- 엔진이 아이템의 어떤 자원을 쓰는지는 `MagicUtilities::GetAssociatedResource` SE 33817 / AE 34609가 정한다(cl:src/RE/M/MagicUtilities.cpp:7-10). 충전 부족은 `CheckCast`(가상표 슬롯 0x0A)가 `kItemCharge`(cl:include/RE/M/MagicSystem.h:19)로 알린다고 IST가 적고 그 지점을 건다(ist:MasterNativeCostHooks.cpp:53-56, :509-516).

가설(확인 방법):
- 비용 0이라 손 충전 액터 값이 줄지 않는다. → T-C5.

### C6. 특기·위력 밸런스

확인됨:
- 풀 SPEL은 SCS가 kOther에서 쏘는 보통 주문이라 SCS 위력 특기(VirtualAV08, scs:include/SpellCastingStaff/CasterDiagnostics.h:10-35, 사용 scs:src/Casting.cpp:489)와 SCS 지팡이 버프(Basic 단계별 능력, scs:src/Equipment.cpp:324-325)가 붙는다.

가설(확인 방법):
- 플레이어 학파 특기가 풀 SPEL에 붙고 바닐라 지팡이 ENCH에는 안 붙는다. → T-C6, 같은 효과를 두 경로로 쏴서 `AdjustActiveEffect` 최종 위력 비교.
- 설정안: (a) 풀 SPEL 위력 배율(효과 복사 때 magnitude × 배율, SCS 수정 불필요), (b) 붙일 Basic 단계(01~05) 고르는 규칙(예: EAMT·가치 기준).

### C7. D:\DEV\StaffCasting(0.1.0)과 겹치는지

확인됨(조사 에이전트 보고, 아래 줄은 그 보고 기준. spike-log 줄은 직접 다시 읽어 확인):
- 목표가 겹친다. StaffCasting도 지팡이 ENCH를 런타임 SPEL로 만들어 AddSpell한다(StaffCasting docs/decisions/0003-staff-spell-forms.md:15, :114-118).
- 장착 훅이 부딪힌다. StaffCasting은 `ActorEquipManager::EquipSpell`(37939/38895), `UnequipObject`(37945/38901) 입구에 Detours를 걸고(src/equip_redirect.cpp:778-785), 플레이어가 아는 kSpell 손 주문을 엔진 대신 자기 칸에 넣는다(0002-equip-hook.md:56-66). 우리 풀 SPEL이 바로 그 조건이다.
- 지팡이 장착 때 ExtraEnchantment를 보고 대리 SPEL을 만든다(0003:109-118). 우리가 붙인 0x80B로 엉뚱한 대리 항목이 생긴다.
- 지팡이 둘을 허용하는 방향이라 SCS(둘이면 하나 해제)와 반대다.
- TAKEALOOK에는 StaffCasting도 SCS도 설치돼 있지 않다(6개 프로필 modlist·plugins 검색 0건; houseCARL로도 SpellCastingStaff.esp 없음 확인).
- 결론: 같이 켜면 안 된다. 우리 모드 설명에 "StaffCasting과 함께 쓰지 말 것"을 적고, 가능하면 DLL이 StaffCasting.dll을 보면 경고만 남긴다. 재사용할 가치가 있는 것은 spike 로그(충전 액터 값 근거)뿐이다.

## 모드 지팡이 호환 (사용자 지시 2026-11-06)

모드팩 기준 실제 대상(houseCARL):
- 애니메이션 종류 Staff인 WEAP 2333개, 그중 마법부여 있는 것 2289개, 정의 플러그인 75개. 많은 순: Staves - Mysticism Consistency.esp 888, ccbgssse066-staves.esl 392, Glenmoril.esm 176, MysticismMagic.esp 115, Saints and Seducers 88, Natura.esp 71, Skyrim.esm 63. 마법부여 없는 44개는 대상 아님.
- 그 ENCH 761개(서로 다른 것): F&F 595 / 집중형 166. Aimed 525 / TargetLocation 212 / TargetActor 18 / Self 6. 효과 2개 이상 641개. 시전 시간 0~2.5초. SCS는 F&F·집중형이면 전달 방식과 상관없이 받는다(scs:src/Equipment.cpp:293-304).
- 스크립트가 붙은 지팡이 7개: Arm of the Sun/Moon(ccpewsse002), Staff of Sheogorath(ccbgssse019), Staff of Worms(ccrmssse001), NMeridia03Staff(Meridia.esp), Hilleye_StaffDreadZombie(HammetDungeon02.esm), aaa_Forsworn__Spear_01(RFM.esm, 이름은 창인데 Staff 애니메이션). 스크립트가 원래 ENCH를 전제로 하면 바꿔 끼울 때 깨질 수 있으니 기본은 제외, 설정으로 켜기.
- 생성기가 거를 것: 마법부여 없음, 비용 0 ENCH(4개), 이미 SCS 키워드 있음, Staff 애니메이션이지만 다른 무기로 보이는 것(목록으로 보고).
- ksws03_quest.esp는 houseCARL이 읽지 못한다(ENCH 1개 미확인).

## 설계 변경 제안 (design.md 수정은 사용자 승인 뒤)

1. 비용 처리(설계 5항의 비용 부분만 교체). "전역 변수 + 효과 조건" 대신 `CastSpellImmediate` 훅 하나:
   - 풀 SPEL이고 충전 ≥ 비용이면 원래 함수 호출 → TargetLocation 실패가 아니면 손 충전 액터 값에서 비용을 뺀다.
   - 충전 부족이면 원래 함수를 부르지 않고 "충전 부족" 알림. 효과는 안 나가고 SCS 발사 소리는 남는다.
   - 집중형: 시작은 위와 같이 허락하고, 매 프레임 kOther가 그 SPEL을 시전 중이면 비용×dt를 손 충전 액터 값에서 뺀다. 바닥나면 `InterruptCast`.
   - "맨손 차단"(풀 SPEL을 손에 직접 장착하는 것 거부)은 그대로 둔다. 손에 직접 든 주문은 `CastSpellImmediate`가 아니라 엔진의 손 시전 과정으로 쏘이고, 지팡이 없는 손의 메뉴 장착은 SCS가 엔진에 넘기기 때문이다(scs:src/Equipment.cpp:38-42).
   - 선택: 충전이 모자라면 풀 SPEL을 잠시 RemoveSpell해서 SCS가 버튼 누를 때 거절하게 할 수 있다(scs:src/Charge.cpp:140-142의 `ValidSpell`이 HasSpell을 봄). SCS는 이때 지팡이-주문 기억을 지우지 않는다(scs:src/Equipment.cpp:148-149, :157-172). 대신 그동안 마법 탭에서 주문이 사라진다.
2. 충전을 빼는 곳: ExtraCharge가 아니라 손 충전 액터 값(`kRightItemCharge`/`kLeftItemCharge`). 장착하지 않은 지팡이의 충전은 건드리지 않는다.
3. 풀 SPEL은 NoAutoCalc 플래그 + 비용 0. 자동 계산이면 SCS가 `CalculateMagickaCost`로 매지카를 받는다(scs:src/Charge.cpp:151, scs:include/SpellCastingStaff/DualCastCost.h:33-35).
4. 비용 = ENIT 숫자가 아니라 런타임 계산값(T-C4로 player/nullptr 중 바닐라와 같은 쪽 고정).
5. 풀 SPEL 자리 수: 설계는 256개인데 이 모드팩에만 서로 다른 지팡이 ENCH가 761개다. 플레이어가 실제로 들어 본 ENCH에만 자리를 주니 보통은 모자라지 않지만, 수집가는 넘길 수 있다. 제안: 1024개로 늘리고(ESL 레코드 한도 안), 가득 차면 소지품에 없는 ENCH의 자리를 비워 재사용.
6. 풀 SPEL 시전 시간 = 원래 ENCH ChargeTime. SCS가 손 공유 ENCH의 시전 시간을 고른 SPEL에 맞추므로(scs:src/Equipment.cpp:214-228) 손맛이 바닐라 지팡이와 비슷해진다.
7. 키워드를 붙이는 방법(사용자 결정 필요): 모드 지팡이 75개 플러그인을 마스터로 거는 ESP는 이 모드팩에서는 되지만 배포용으로는 쓸 수 없다(사람마다 모드 목록이 다름). 배포까지 생각하면 런타임 부여(우리 DLL 또는 KID)가 필요하다. SCS는 kDataLoaded에 한 번만 지팡이 목록을 만들므로(scs:src/Controller.cpp:69, :112-121) 우리 부여가 그보다 먼저 끝나야 한다. 순서는 미확인 → 자가 테스트에서 두 DLL의 kDataLoaded 처리 시각과 SCS 로그의 "N keyword staffs" 수를 비교.
8. 알아 둘 부작용: SCS는 지팡이 base WEAP 이름을 고른 주문 이름으로 바꾼다(scs:src/Equipment.cpp:59-60). base 단위라 같은 종류 지팡이 전부(NPC 것 포함) 이름이 바뀌어 보인다. 화면에만 영향.

## 자가 테스트 목록 (4·5단계에서 구현)

| ID | 무엇 | 자동/사용자 | 판정 |
|---|---|---|---|
| T-C2a | 장착 직후 그 항목 ENCH = 0x80B | 자동(장착 이벤트) | PASS/FAIL 로그 |
| T-C2b | 첫 공격에 손 캐스터 currentSpell = 0x80B (SCS 로그에 `native cast never started`가 없음) | 사용자 공격 1회 | 로그 |
| T-C3 | 변환 전후 충전 액터 값·최대치 동일, 해제 뒤 ExtraCharge가 액터 값과 같음 | 자동 + 해제 1회 | PASS/FAIL |
| T-C4 | 시험용 지팡이 1발: `currentSpellCost`·충전 차이 vs 계산 비용 2종 | 사용자 공격 1회 | 로그 |
| T-C5 | 변환 지팡이 1발 동안 엔진 쪽 충전 액터 값 변화 = 0(우리 차감 제외) | 사용자 공격 1회 | PASS/FAIL |
| T-C1 | 충전 0에서 발사 차단, 효과 없음 / 충전 충분하면 비용만큼 감소 | 사용자 공격 2회 | PASS/FAIL |
| T-C6 | 같은 효과 위력 비교(시험용 vs 변환) | 사용자 공격 2회 | 로그 |
| T-ORD | 키워드 부여가 SCS 지팡이 목록 작성보다 먼저인지 | 자동(시작 시) | PASS/FAIL |

사용자 공격이 필요한 항목은 한 번의 테스트 세션에 몰아서 한다.
