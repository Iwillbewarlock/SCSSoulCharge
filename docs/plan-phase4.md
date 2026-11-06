# 4단계 DLL 구현 계획 (초안, 2026-11-06)

설계: docs/design.md. 근거: docs/phase0-findings.md. 이 문서는 "무엇을 어떤 순서로 만들지"만 다룬다.

## 빌드 환경 (StaffCasting과 같은 방식)
- CMake + Ninja 프리셋, MSVC(VS 18), vcpkg `C:/Dev/vcpkg`, 트리플렛 x64-windows-static-md. 빌드 출력은 C: 공간 때문에 `D:/DEV/build/SCSSoulCharge`.
- CommonLibSSE-NG: `extern/CommonLibSSE-NG`(StaffCasting과 같은 커밋 8f4205da5를 복사). SE+AE 켜고 VR 끔.
- 대상 런타임: 1.5.97 / 1.6.1170 / 1.7.104. SCS처럼 시작 때 실행 파일 버전을 확인하고 그 밖이면 아무 훅도 걸지 않는다.
- 가설: 이 CommonLib이 1.7.104의 Address Library 파일을 읽는다. 확인 방법: 빌드 뒤 1.7.104 로그의 "버전·주소 확인 PASS" 줄. SCS는 1.7.104용 CommonLib을 따로 고쳐 썼다(scs CMakeLists.txt "1.7.104-backported").

## 파일 구성
게임과 무관한 판단은 `core/`에 두고 오프라인 테스트로 확인한다(StaffCasting과 같은 구조).

| 파일 | 하는 일 | 확인 방법 |
|---|---|---|
| core/staff_filter.* | 지팡이 후보 판정(마법부여 있음, 비용 > 0, SCS 키워드 없음, 스크립트 지팡이 기본 제외, INI 포함·제외 목록) | 오프라인 테스트 |
| core/pool_table.* | ENCH ↔ 자리 번호 표: 처음 본 ENCH에 배정(재사용 없음), 가득 차면 거부, 코세이브 바이트로 저장·복원, 깨진 데이터 거부 | 오프라인 테스트 |
| core/charge_rules.* | 발사 허락/거부, 차감량, TargetLocation 실패면 차감 안 함, 집중형 매 프레임 차감과 중단 판정 | 오프라인 테스트 |
| src/plugin.cpp | 시작, 로그, 버전 확인, 메시지 처리(kDataLoaded 등) | 로그 |
| src/settings.* | INI 읽기(이름 지키기, 위력 배율, Basic 단계 규칙, 제외·포함 목록, 자가 테스트 켜기) | 로그 |
| src/keywords.* | kDataLoaded에 지팡이 WEAP에 SCS 키워드 두 개를 붙임. 끝난 시각·개수를 로그(T-ORD) | T-ORD |
| src/pool.* | 풀 SPEL 1024개 찾기, ENCH의 효과 포인터를 공유해 채우기(시전 종류·시간·전달 방식·이름 설정) | T-ESP, T-POOL |
| src/conversion.* | 플레이어가 장착한 지팡이 항목에 공유 ENCH를 ExtraEnchantment로 붙이고(최대 충전 = base EAMT), 풀 SPEL AddSpell. 해제 때 되돌리고 RemoveSpell | T-C2a, T-C3, T-REV |
| src/cast_hook.* | ActorMagicCaster 가상표 슬롯 0x01 훅: 풀 SPEL 발사 허락·거부, 손 충전 액터 값 차감 | T-C1, T-C5 |
| src/frame.* | 프레임 훅(SCS와 같은 Main::Update call 지점에 사슬): 집중형 차감·중단, 이름 지키기 | T-C1, T-NAME |
| src/bare_hand.* | 풀 SPEL이 맨손에 장착되면 바로 해제하고 알림(장비 이벤트 방식, 훅 없음) | T-BARE |
| src/serialization.* | 코세이브: 자리 표, 버전 필드 | T-SAVE |
| src/selftest.* | INI로 켜는 자가 테스트, 결과를 PASS/FAIL 줄로 로그 | Claude가 로그 판정 |
| tests/*.cpp | core 오프라인 테스트(각 모듈 정상·경계·실패 최소 3개) | 빌드 때 실행 |

## 만드는 순서
1. 뼈대: CMake·프리셋·build-local.cmd, plugin.cpp(버전 확인·로그만), 빈 core와 테스트 실행. 빌드 성공 확인.
2. core 3개와 오프라인 테스트.
3. settings + selftest 틀 + keywords(InitItem 훅). 처음에는 INI 포함 목록의 시험용 지팡이 하나에만 붙인다. (T-ORD 첫 확인 지점)
4. pool + conversion + serialization. 관문: T-REV(해제 뒤 ExtraCharge 보존) 결과로 되돌림 방식 확정.
5. cast_hook + frame + bare_hand.
6. 나머지 자가 테스트, StaffCasting.dll 감지 경고. 여기까지 모두 통과한 뒤에야 전체 지팡이 키워드 부여를 켠다.
7. 리뷰: reviewer 에이전트 + GPT 리뷰, 맞는 지적만 반영 후 2차 리뷰.
8. MO2 배포 폴더 만들기(새 모드 폴더, 사용자가 켬)와 사용자 인게임 확인 목록.

## 주의할 점
- 되돌림(해제·버리기·거래·동료에게 주기): ExtraEnchantment의 `removeOnUnequip` 플래그(cl:include/RE/E/ExtraEnchantment.h:25)를 켜면 엔진이 해제 때 마법부여를 떼는지 먼저 시험한다. 그렇다면 해제가 곧 되돌림이라 다른 경로를 따로 막을 필요가 줄어든다. 가설 → T-REV. 아니면 해제 이벤트에서 직접 뗀다.
- 차감 방법: `RestoreActorValue(kDamage, k(Left|Right)ItemCharge, -비용)`. 해제 뒤 ExtraCharge에 반영되는지 T-C3에서 확인.
- 어느 손 지팡이에서 뺄지: SCS는 SCS 지팡이를 하나만 허용하므로(scs:src/Equipment.cpp:95-130), 풀 SPEL에 대응하는 ENCH를 가진 변환 지팡이가 든 손이 하나뿐이다.
- 1.7.104에서 가상표 슬롯 번호가 바뀐 예가 있다(SCS 공격 처리 슬롯 0x04 → 0x06, scs:include/SpellCastingStaff/RuntimeCompatibility.h:22). ActorMagicCaster 슬롯 0x1C는 SCS가 버전 구분 없이 쓰고 1.7.104에서 동작 보고가 있다(scs README.md:37). 슬롯 0x01도 같을 것으로 보지만 가설 → 시작 때 슬롯 0x01 대상이 게임 실행 파일 안 함수인지 확인하고 아니면 훅을 걸지 않는다.
- 맨손 차단은 훅 없이 장비 이벤트로 한다. 가설: 한 프레임 손에 주문이 보일 수 있다(눈으로만 판단).
- 테스트에는 SCS 2.1.1이 MO2에 설치·활성화돼 있어야 한다. 지금 TAKEALOOK에는 설치돼 있지 않다(houseCARL 확인). 받아 둔 압축은 `D:/TAKEALOOK/downloads/Spellcasting Staff  Korean ver-196207-2-1-1-...zip`.

## 계획 검토에서 바꾼 것 (advisor, 2026-11-06)
1. 자리 비우기(재사용) 없음. SCS가 base WEAP → SPEL을 자기 코세이브에 기억하므로(scs:src/Equipment.cpp:57-58, :157-172) 자리를 다른 ENCH에 다시 주면 SCS 기억이 엉뚱한 주문을 가리킨다. 한 세이브 안에서는 처음 배정한 자리를 끝까지 유지하고, 1024개가 차면 로그·알림 후 변환을 거부한다(이 모드팩 ENCH 761개 < 1024). design.md 3항도 같이 고친다.
2. `removeOnUnequip` 시험(T-REV)은 4번 단계의 관문이다. 해제 뒤 ExtraCharge가 남아 있고 그 값이 깎인 충전 액터 값과 같아야 PASS. 엔진이 마법부여와 충전을 같이 떼면 해제·재장착마다 충전이 가득 차는 구멍이 생기므로, 그때는 이 플래그를 쓰지 않고 해제 이벤트에서 직접 되돌린다.
3. 효과는 복사하지 않고 원래 ENCH의 `Effect*`를 공유한다(가장 단순, 메모리 할당 없음). 그래서 위력 배율(design 7항)은 이번 범위에서 뺀다(공유한 효과의 크기를 바꾸면 원래 지팡이도 바뀜). SCS 위력 특기가 이미 붙는다. design.md 3·7항을 같이 고친다.
4. 로드 순서: 런타임으로 채운 SPEL은 저장되지 않는다. kPostLoadGame에 코세이브의 자리 표를 읽은 즉시 다시 채운다. 장착 때는 우리 장비 이벤트 안에서 바로 채우고 AddSpell한다. SCS는 장비 이벤트에서 Sync를 프레임 훅으로 미룬다(scs:src/Events.cpp:19-20 `QueueSync()`). 따라서 SCS의 ENCH 동기화(scs:src/Equipment.cpp:214-228)는 이미 채워진 시전 종류·시간을 본다.
5. 집중형을 우리가 끊으면 SCS가 kOther 채널을 정리하지만(scs:src/Casting.cpp:895-898) 손의 가짜 ENCH 애니메이션은 버튼을 뗄 때까지 남는다. SCS의 Rapid처럼 손 캐스터도 그 가짜 ENCH일 때만 끊는다(scs:src/Casting.cpp:692-693 방식). T-C1에 넣는다.
6. 가설: 플레이어 `AddSpell`(SE 37771 / AE 38716)이 "주문을 배웠다" 알림을 띄울 수 있다. 장착마다 뜨면 안 되므로 T-POOL에서 확인하고, 뜨면 알림 없는 경로를 찾는다.
7. CommonLib은 StaffCasting 쪽 한 벌로 고정하고, 쓰는 ID를 그 사본에서 다시 확인했다: ActorMagicCaster 가상표 257613/205828(Offsets_VTABLE.h:1862), InterruptCast 33630/34408(MagicCaster.cpp:40), CalculateCost 11213/11321(MagicItem.cpp:47), AddSpell 37771/38716(Actor.cpp:94), `BGSKeywordForm::AddKeyword`(BGSKeywordForm.h:29). SCS의 RVA 대조표는 가져오지 않는다. 버전 확인은 실행 파일 버전 + Address Library ID로만 한다.

## GPT 설계 검토 반영 (2026-11-06, 지적 8건 중 7건 맞음, 1건 다른 판단)
1. 맞음(확인): StaffCasting의 CommonLib(8f4205da5)은 1.7.x를 SE로 판정하고(include/REL/Module.h:339-347 `case 6`만 AE) 주소 형식 5를 못 읽는다. → 의존성을 다시 고른다(사용자 결정, 아래 "결정 필요").
2. 맞음(확인): 그 CommonLib의 `RestoreActorValue`는 인자 2개이고 절댓값을 써서 항상 회복 방향이다(src/RE/A/ActorValueOwner.cpp:28-33). 차감은 `ModActorValue(kDamage, k(Left|Right)ItemCharge, -양)`(include/RE/A/ActorValueOwner.h:21)으로 한다.
3. 맞음: SKSE가 DLL을 이름순으로 부른다는 보장이 없다(폴더 열거 순서, MO2 가상 폴더에서는 더 불확실). 키워드 부여 시점을 바꾼다: 무기 폼 초기화 함수 `TESObjectWEAP::InitItemImpl`(가상표 슬롯 0x13, cl:include/RE/T/TESForm.h:144, 가상표 ID SE 234396 / AE 189786 cl:include/RE/Offsets_VTABLE.h:557)을 플러그인 로드 때 훅으로 걸고, 원래 함수가 끝난 직후 그 무기에 키워드를 붙인다. 폼 초기화는 kDataLoaded 알림보다 먼저 끝나므로 SCS 목록 작성보다 앞선다(가설, T-ORD로 확인). 안전장치: kDataLoaded에 우리 부여 개수와 SCS 키워드가 붙은 WEAP 수를 세어 기록한다. kInputLoaded는 폼 로드 완료 보장이 없어 쓰지 않는다.
4. 맞음: 준비가 안 됐으면 아무것도 바꾸지 않는다. InitItem 훅 안에서 키워드를 붙이기 전에 SpellCastingStaff.esp와 SCSSoulCharge.esp가 로드 목록에 있고, 런타임 버전 확인과 필수 훅(발사 훅·프레임 훅) 설치가 성공했는지 본다. 하나라도 아니면 키워드를 하나도 안 붙인다(키워드만 붙고 변환이 안 되면 SCS가 그 지팡이 공격을 막아 지팡이가 못 쓰게 됨, scs:src/Charge.cpp:128-142). 개발 순서도 바꿔, 전역 키워드 부여는 발사 훅·차감·맨손 차단이 다 된 뒤 켠다. 그 전 실험은 시험용 지팡이 하나(INI 포함 목록)로만 한다.
5. 맞음: 코세이브 상태 전이를 정의한다. Revert(새 게임·다른 세이브 로드 시작) → 표 비움, 시전 차단. Load 성공 → FormID 재해석(ResolveFormID) 후 표 적용, 풀 다시 채움, 장착 중인 지팡이 재점검 후 차단 해제. 코세이브 없음·손상 → 기존 자리를 임의 배정하지 않고, 플레이어가 가진 풀 SPEL과 SCS 기억이 가리키는 풀 SPEL만 "알 수 없음"으로 두고 변환을 막은 채 로그·알림. 우리 변환 항목 식별: base가 SCS 지팡이가 아닌데 ExtraEnchantment가 공유 ENCH(0x80B)이면 우리 것(사용자 마법부여와 구분). T-SAVE에 세이브 A→B 전환, 새 게임, 코세이브 삭제 경우를 넣는다.
6. 맞음(확인): `MagicItem`에는 효과 목록과 따로 `hostileCount`, `avEffectSetting`이 있다(cl:include/RE/M/MagicItem.h:129-135). 채울 때 ENCH의 값을 같이 옮긴다. T-POOL에서 `IsHostile()`과 AV 효과가 원래 ENCH와 같은지 비교.
7. 맞음: 집중형 비용은 초당 값이다(scs:src/Casting.cpp:759-761). 집중형은 충전 > 0이면 시작하고, 실제 진행 시간만큼만 뺀다. 시작할 때 한 번에 빼지 않는다. 일시정지·큰 dt(SCS처럼 0.25초 넘으면 끊기)·고갈 직후를 테스트.
8. 다른 판단: GPT는 효과를 독립 복사하라고 했지만 공유로 간다. 근거: 지속 효과(ActiveEffect)는 `MagicItem*`와 `Effect*`를 들고 있는데(cl:include/RE/A/ActiveEffect.h:104-105), 공유한 `Effect*`의 주인은 게임 내내 사라지지 않는 원래 ENCH라 끊어진 포인터가 생기지 않는다. 우리는 자리를 재사용하지 않으므로(위 1번) SPEL 쪽도 바뀌지 않는다.
- 슬롯 0x01: 배포된 SCS 소스의 백포트 CommonLib도 같은 선언이다(GPT 확인). 1.7.104 실제 동작은 미확인 → 런타임별로 훅이 처음 불릴 때 인자(주문 FormID, 캐스터 출처)를 로그로 남겨 함수 정체를 확인한다.
- SCS 재동기화: SCS는 장비 이벤트 때만 Sync를 예약한다(scs:src/Events.cpp:19-20). AddSpell 직후 SCS가 기억을 다시 읽게 하려면 장비 이벤트가 필요할 수 있다 → T-POOL에서 "지급 직후 첫 공격이 막히지 않음"을 확인.

## 결정 필요: 1.7.104를 받칠 CommonLib
| 선택 | 내용 | 장점 | 단점 |
|---|---|---|---|
| A. alandtse CommonLibVR 10.1.0 (C:/DEV/CausticLight에 있음, 39f9d07) | 1.7.99+ 주소 형식 5와 AE 판정을 정식 지원(include/REL/Module.h:341-347, include/REL/IDDB.h:220) | 1.7 구조 변경을 계속 따라감 | GPL-3(모드 예외 조항 있음). 우리 DLL 소스를 공개해야 함 |
| B. SCS가 같이 배포한 백포트 CommonLib (SCS 소스 압축 vendor/) | CharmedBaryon 3.6.0 + 형식 5 읽기만 추가, MIT | SCS와 같은 기반, MIT | "SCS가 쓰는 경로만 점검했다"고 명시(SCS_BACKPORT.md). 우리가 쓰는 SpellItem·ExtraEnchantment·코세이브 등의 1.7 구조는 직접 점검해야 함 |
| C. 1.7.104는 나중에 | 처음엔 1.5.97·1.6.1170만 | 가장 단순 | 1.7.104 사용자는 못 씀 |

권장: A. 1.7 구조 차이를 라이브러리 쪽이 관리하므로 위험이 가장 작다. 소스 공개는 GitHub에 올리면 된다.

## 사용자 인게임 확인 (자동화 못 하는 것만)
- 지팡이 공격 입력이 필요한 항목(T-C1, T-C2b, T-C4, T-C5, T-C6): 플러그인이 지팡이 공격을 대신 일으키는 방법을 찾지 못함. 한 테스트 세션에서 몰아서 한다. 판정은 로그로 Claude가 한다.
- 화면: 이름 깜빡임, 맨손 차단 때 손에 주문이 한 프레임 보이는지, 충전 막대·재충전 화면 모양.
- 손맛: 시전 시간이 바닐라 지팡이와 비슷한지.

## 3·4단계 리뷰 뒤 남긴 것 (2026-11-06)
리뷰: reviewer 에이전트 9건, GPT 1차 7건(겹침 포함), GPT 2차(수정 확인 5건 해결, 미해결 2건, 새 지적 3건). 수정 커밋 bcc652a와 그 뒤 커밋.
- 받아들인 위험(GPT #2): 코세이브에 POOL 레코드가 없고 플레이어가 풀 주문을 하나도 모르면 새 표로 시작한다. POOL과 SCS 기억은 같은 .skse 파일에 함께 저장되므로, POOL만 빠지는 경우는 우리 모드 없이 만든 세이브(그때 SCS 기억은 우리 FormID를 풀지 못해 남지 않음)이거나 코세이브 쓰기 실패뿐이라고 판단했다. 추정이며 T-SAVE에 "모드 끄고 저장 → 다시 켜고 로드" 경우를 넣어 확인한다.
- 알려진 한계(GPT #5): 같은 base 지팡이 중 다른 마법부여가 따로 붙은 복사본은 변환하지 않는다. SCS는 base 키워드만 보므로 그 복사본은 SCS 촉매로 취급되지만 주문이 없어 공격이 막힌다(SCS 알림 표시). 바닐라에서는 마법부여된 지팡이에 다시 마법부여할 수 없어 드문 경우로 본다. 모드 설명에 적는다.
- 가설 T-EXIT: 풀 SPEL이 원래 ENCH의 Effect*를 공유하므로, 게임 종료 때 엔진이 두 아이템의 효과를 모두 해제하면 종료 시 충돌이 날 수 있다(근거 없음, 두 리뷰 모두 미확인). 확인: 변환 지팡이를 쓴 뒤 게임 종료, 크래시 로그가 생기지 않으면 PASS. 생기면 효과를 독립 복사하는 방식으로 바꾼다.
- 주문 지급 순서(GPT 2차 새 가설): 장착 이벤트 안에서 바로 AddSpell하고, 마법부여 붙이기와 정리만 다음 작업으로 미뤘다. SCS는 같은 이벤트에서 Sync를 다음 프레임으로 미루므로 SCS가 주문을 찾을 때 이미 배운 상태다(추정, T-POOL에서 "장착 직후 첫 공격이 막히지 않음"으로 확인).

## 5단계 리뷰 뒤 (2026-11-06)
GPT 3건 + reviewer 9건(겹침 포함) → 11건 수정(커밋 이 줄 다음). 고친 것: 한 발 주문은 엔진 호출 전에 차감하고 룬·소환 실패면 돌려줌(소환무기 주문이 지팡이를 내려놓는 경우), 집중형은 그 손에 지팡이가 그대로일 때만 차감, 엔진이 시작을 거절하면 채널 기록 안 함, Spell Hotbar 2가 kOther를 잡으면 차감·중단 없이 손 뗌, 이름 되돌린 뒤 인벤토리 목록 다시 그림, 아이템 개별 이름도 되돌림, T-NAME 추가, 시전 훅 설치 때 슬롯 대상·첫 호출 로그, 맨손 차단을 매 프레임·로드 뒤에도 검사, T-C1 거부 판정을 캐스터 상태로, 큰 프레임 공백은 끊지 않고 그 프레임만 차감 생략.
- 가설 T-PERF(reviewer 범위 밖 관찰): 전체 지팡이 키워드 부여를 켜면 SCS 지팡이 목록이 약 2300개가 되고, SCS의 이름 갱신(Naming.cpp:53-75)이 장비를 바꿀 때마다 그 목록 × 소지품을 돈다. 장비 교체 때 끊김이 있는지 6단계에서 프레임 시간으로 잰다.
- 가설: `Actor::DeselectSpell`(37820/38769)이 맨손 주문을 실제로 내리는지는 T-BARE 로그로만 판정된다.

## 진행 상태 (2026-11-06)
1~6단계 구현 완료, 단계마다 리뷰 반영(7단계는 단계별로 수행). 마지막 커밋 2e0ad4a. 8단계: 시험판 파일은 `D:\DEV\build\SCSSoulCharge\mod-staging`에 준비, MO2 반영은 게임·MO2를 끈 뒤 모드 관리 case로 한다. 시험 절차와 판정표는 docs/ingame-test.md. Release 빌드에 PDB가 없어 크래시 로그의 우리 DLL 프레임은 이름 없이 나온다(필요하면 RelWithDebInfo로 다시 빌드).

## 1차 게임 안 시험 결과 (2026-11-06, 에이전트가 DevBench로, Mod-Management case 281 §11)
- 확인됨: T-ORD PASS(SCS "ready: 28 keyword staffs" = 27 + 변환 1). 키워드 부여 시점 가설이 맞음.
- 확인됨: T-ESP, T-SAVE, T-BARE, T-C3 PASS. 장착 시 변환·충전 주문 지급·해제 시 회수 동작.
- 확인됨 FAIL: T-VMAD — `HasVMAD()`가 모든 지팡이에 참. 스크립트 지팡이 판정을 다른 방법(플러그인 파일의 VMAD 하위 레코드 직접 확인 등)으로 바꿔야 함. 그때까지 시험판은 목록 지정 방식이라 영향 없음.
- 확인됨 FAIL: 버린 지팡이에 공유 마법부여가 남음 → 다음 작업에서 처리하도록 고침(커밋 이 줄 직전), 재시험 필요.
- 미확인: T-C2a의 엔진 조회는 원래 마법부여를 돌려줌(착용 아이템 자체에는 공유 마법부여가 붙어 있음). 실제 시전에 쓰이는 마법부여(T-C2b)와 발사·차감(T-C1, T-C4, T-C5), 이름 지키기는 공격 입력과 마법 메뉴 지정이 필요해 사용자 시험으로 남김.

## 사용자 직접 시험 결과 (2026-11-06 22:17~22:27, 로그로 판정)
- 확인됨 PASS T-C4: 바닐라 시험용 지팡이 한 발 소모 117.30 = `CalculateMagickaCost(player)` 117.30 (nullptr 133.04, 레코드 60은 불일치). 비용은 `CalculateMagickaCost(player)`로 고정.
- 확인됨 FAIL T-C2b: 변환 지팡이로 쏠 때 손 캐스터가 원래 ENCH(0004C6CB)로 시전. SCS는 `native cast never started`로 포기하고, 엔진이 지팡이 원래 마법을 쏨(사용자 화면: "시전은 여전히 기본 지팡이 마법"). 지정 주문을 바꿔도 같음.
- 근거: 같은 아이템의 착용 목록에는 공유 ENCH가 붙어 있는데(Papyrus `WornObject.GetEnchantment` = FEFA680B), 엔진 `InventoryEntryData::GetEnchantment`(15788/16026)는 원래 ENCH를 돌려줌(T-C2a FAIL, 매번).
- 가설 H2(유력): 엔진의 아이템 마법부여 조회는 base 레코드에 마법부여가 있으면 그것을 먼저 쓴다(바닐라엔 둘 다 있는 경우가 없음). 확인 방법: 조회 함수를 훅해 우리 표식이 있으면 공유 ENCH를 돌려주게 한 뒤 T-C2a·T-C2b가 PASS로 바뀌는지.
- 가설 H1(약함): 손 캐스터가 장착 순간 마법부여를 정해 두고 다시 보지 않는다. 약한 이유: T-C2a는 장착 뒤 순수 조회인데도 원래 ENCH를 돌려줌.
