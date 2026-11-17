제공해주신 코드 발췌본과 로그, 그리고 Skyrim AE(1.6.1170) 엔진 및 CommonLibSSE-NG 지식을 바탕으로 진단한 결과입니다. 도구 호출이나 파일 읽기는 수행하지 않았으며, 모든 기술적 주장에 `[CERTAIN]` 또는 `[GUESS]`(헤더 직접 대조가 불가능한 내부 동작은 `미확인` 병기)를 표시했습니다.

---

## 1. 같은 프로세스 재로드에서는 유지되고 게임 재시작 후에는 사라지는 원인

### 핵심 요약
7개의 `ActiveEffect` 자체는 `.ess` 세이브 파일에 정상적으로 기록되었습니다 `[CERTAIN]`. 같은 프로세스에서 다시 불러올 때는 힙 메모리의 전역 폼 테이블에 원본 `0xFF0039E6` 객체와 `effects` 배열이 그대로 남아 있어 복원에 성공하지만, 게임을 완전히 껐다 켜면 `CopySpellToEnchantment`의 `enchantment->AddChange(1)`(`testcontrols.cpp` `CopySpellToEnchantment` 8행)로 인해 **`effects` 배열이 비어 있는 빈 껍데기 `EnchantmentItem`**만 복원되기 때문에 `ActiveEffect` 역직렬화 단계에서 7개 효과가 모두 버려집니다.

### 단계별 엔진 메커니즘

1. **런타임 복사본 생성과 `AddChange(1)`의 실제 동작**
   - `RE::IFormFactory::GetConcreteFormFactoryByType<RE::EnchantmentItem>()->Create()`(`CopySpellToEnchantment` 3~4행)는 엔진 힙에 `RE::EnchantmentItem`을 할당하고 `0xFF000000` 이상의 동적 FormID(`0xFF0039E6`)를 부여한 뒤 전역 해시맵인 `RE::TESForm::GetAllForms()`에 등록합니다 `[CERTAIN]`.
   - 이어서 호출한 `enchantment->AddChange(1)`(`CopySpellToEnchantment` 8행)은 `RE::BGSSaveLoadGame`에 변경 플래그 `0x1`(`CHANGE_FORM_FLAGS`)만 표시합니다 `[CERTAIN]`.
   - `RE::EnchantmentItem`(상속 경로는 `RE::MagicItem` -> `RE::TESBoundObject` -> `RE::TESObject` -> `RE::TESForm`)은 `TESForm::SaveGame`과 `TESForm::LoadGame`을 오버라이드하여 `MagicItem::effects`(`RE::BSTArray<RE::Effect*>`)나 `EnchantmentItem::Data`를 ChangeForm에 직렬화하지 않습니다 `[CERTAIN]`. 플래그 `1`은 오직 `TESForm::formFlags`만 저장합니다 `[CERTAIN]`. 엔진이 동적 `EnchantmentItem`의 `effects` 목록을 세이브에 저장하는 경로는 마법부여대가 쓰는 `RE::BGSCreatedObjectManager`(세이브 Global Data Type 6)뿐인데, 첨부 코드는 이 매니저에 등록하지 않았습니다 `[CERTAIN]`.
   - 또한 `CopySpellToEnchantment`(1~29행)는 `TESFullName::fullName`을 복사하지 않으므로 첫 프로세스의 세이브 시점 로그(`01:11:08.851`)에서도 이미 이름이 빈 문자열(`''`)로 찍힙니다 `[CERTAIN]`.

2. **`ActiveEffect`가 세이브에 기록되고 읽히는 방식**
   - 플레이어(`0x00000014`)의 Actor ChangeForm을 저장할 때 엔진은 `RE::MagicTarget`의 활성 효과 목록과 각 `RE::ActiveEffect::SaveGame(RE::BGSSaveFormBuffer*)` 데이터를 기록합니다 `[CERTAIN]` (외부 리스트 직렬화 함수의 정확한 내부 심볼명은 `미확인` `[GUESS]`).
   - 첨부 로그에서 Save A(`01:11:08.851`)의 `elapsed=6.6`이 이후 Save B 저장 뒤 Save A를 다시 불러왔을 때(`01:11:28.797`) 정확히 `elapsed=6.6`으로 복원된 점은 7개의 `ActiveEffect` 레코드가 `.ess` 파일 내부에 실제로 기록되었음을 증명합니다 `[CERTAIN]`.
   - 결정적인 이유는 `RE::ActiveEffect::effect`(`RE::Effect*`)의 구조에 있습니다. `RE::Effect`(`CopySpellToEnchantment` 12행의 `new RE::Effect()`)는 `RE::TESForm` 파생 클래스가 아닌 일반 힙 구조체이므로 고유 FormID가 없습니다 `[CERTAIN]`. 따라서 엔진은 `ActiveEffect`를 저장할 때 `ActiveEffect::spell`(`RE::MagicItem*`)의 FormID(`0xFF0039E6`)와 **그 `spell->effects` 배열 안에서의 인덱스(`0..N-1`)**를 저장합니다 `[GUESS — 바이너리 직렬화 포맷 세부 구조는 헤더 밖 영역이라 미확인이나, `RE::Effect`에 FormID가 없다는 CommonLib 구조상 필수적인 동작]`.
   - 세이브를 읽을 때 엔진은 먼저 `spell` FormID(`0xFF0039E6`)를 `RE::TESForm::LookupByID` 계열 경로로 찾고, `spell->effects[index]`에서 `RE::Effect*`와 `Effect::baseEffect`(`RE::EffectSetting*`)의 아키타입을 확인해야 알맞은 `ActiveEffect` 파생 클래스(`ValueModifierEffect` 등)를 생성하고 `ActiveEffect::LoadGame(RE::BGSLoadFormBuffer*)`을 호출할 수 있습니다 `[GUESS — 정확한 내부 팩토리 함수명은 미확인]`.

3. **같은 프로세스 재로드에서 살아남는 이유**
   - 게임을 끄지 않고 세이브를 다시 불러올 때 엔진의 Revert 과정은 월드 레퍼런스(`TESObjectREFR`, `Actor`, `Projectile`)와 `BGSCreatedObjectManager` 관리 대상만 정리하며, 플러그인이 `IFormFactory::Create()`로 직접 만들어 둔 비관리 `EnchantmentItem`은 `RE::TESForm::GetAllForms()`에서 지우지 않습니다 `[CERTAIN — 첨부 재로드 로그 `01:11:28.797`로 입증]`.
   - 따라서 `PlayerCharacter::LoadGame` 시점에 `0xFF0039E6`을 조회하면 메모리에 살아 있던 기존 `EnchantmentItem`(8개의 `RE::Effect*` 보유)이 그대로 반환되어 `selectedSpells[kRightHand]`와 7개 `ActiveEffect` 모두 정상 복원됩니다 `[CERTAIN]`.

4. **완전 재시작 후 7개 효과가 사라지는 이유와 `slot=FF0039E6`의 정체**
   - `LogSaveState`(`LogSaveState` 발췌부 6~9행)에서 `slot`은 FormID 정수가 아니라 `const RE::MagicItem*` 포인터이며, `slot->GetFormID()`와 가상 함수 `slot->GetName()`을 호출해도 크래시가 나지 않고 `FF0039E6 ''`을 출력했습니다 `[CERTAIN]`. 즉 재시작 직후 새로 뜬 프로세스인데도 `0xFF0039E6`을 가진 실제 `TESForm` 객체가 `PlayerCharacter::LoadGame` 이전에 힙에 생성되어 `GetAllForms()`에 등록되었다는 뜻입니다 `[CERTAIN]`.
   - 이는 `enchantment->AddChange(1)`이 `.ess`의 ChangeForm 테이블에 `ENCH`(`FormType::Enchantment`, 마법부여 해금 시 `kPlayerKnows` 저장용으로 ChangeForm 타입 테이블에 존재) 레코드를 남겼고, 재시작 후 ChangeForm 로더가 `0xFF0039E6`에 대해 빈 `EnchantmentItem`을 생성한 뒤 `TESForm::LoadGame`만 실행했기 때문입니다 `[GUESS — ChangeForm 로더의 동적 `ENCH` 인스턴스화 분기는 엔진 내부 코드라 미확인이나, Save A/B 반복 로드에서 항상 크래시 없이 `slot=FF0039E6 ''`이 찍히는 현상과 정확히 부합]`.
   - 이 빈 껍데기 `EnchantmentItem`은 **`effects` 배열 길이가 0**이므로, `PlayerCharacter::LoadGame`이 `selectedSpells[kRightHand]` 포인터는 이 객체로 연결하지만 `MagicTarget`이 7개 `ActiveEffect`의 `spell->effects[index]`를 찾을 때는 배열이 비어 있어 생성을 포기하고 7개 모두 건너뜁니다(크래시 없음, `578 -> 571`, `592 -> 585`) `[GUESS]`.
   - 만약 `AddChange(1)`조차 호출하지 않아 세이브에 아예 기록되지 않은 `0xFF` 폼이라면, 재시작 후 `LookupByID`가 `nullptr`을 반환하여 `slot`은 `00000000`(`nullptr`)이 되고 7개 효과 역시 `spell == nullptr`로 버려집니다 `[CERTAIN]`.

5. **재시작 후 손 슬롯의 `FF0039E6`이 위험한 이유**
   - **시전 시 크래시 및 상태 꼬임**: 빈 껍데기 `EnchantmentItem`은 `effects`가 비어 있어 `MagicItem::GetCostliestEffectItem()`이 `nullptr`을 반환하며 `[CERTAIN]`, `EnchantmentItem::Data`도 초기값(`SpellType::kEnchantment`, `CastingType::kConstantEffect`, `Delivery::kSelf`)으로 리셋됩니다 `[CERTAIN]`. 이 상태로 공격 버튼을 누르면 유효한 효과를 가정하는 훅이나 `ActorMagicCaster`에서 널 역참조 크래시(CTD)가 나거나 시전 상태가 먹통이 될 수 있습니다 `[GUESS]`.
   - **영구 세이브 비대화(Save Bloat)**: `AddChange(1)`이 남아 있는 한 주문이나 지팡이를 바꿀 때마다 새로운 `0xFFxxxxxx` ChangeForm이 `.ess`에 영구 누적되며 모드를 제거해도 사라지지 않습니다 `[CERTAIN]`.
   - **동적 FormID 재할당 충돌(Type Confusion)**: `AddChange(1)`을 빼더라도 같은 프로세스에서 **이전에 저장한 세이브**를 불러오면 세이브의 다음 동적 FormID 카운터가 과거 값으로 되돌아갑니다 `[GUESS — 내부 카운터 변수명은 미확인]`. 이때 `GetAllForms()`에 이전 타임라인의 `0xFF` 폼이 그대로 남아 있거나, 반대로 로드 중 다른 동적 폼(`TESObjectREFR`, `Projectile` 등)이 우연히 같은 `0xFF` 번호를 먼저 받으면 포인터 타입 혼동으로 즉시 CTD가 발생할 수 있습니다 `[GUESS]`.

---

## 2. 재시작 후에도 7개 효과를 유지하는 해결책 (신뢰도 순위)

### 1순위 (가장 권장): 적용된 `ActiveEffect`를 원본 `RE::SpellItem`으로 재지정 + `AddChange(1)` 제거
지팡이 발사 자체는 런타임 `EnchantmentItem` 복사본으로 수행하되, 효과가 대상(`MagicTarget`)에 들어갈 때 `ActiveEffect`의 `spell`과 `effect`를 `0xFF` 복사본 대신 **`TESDataHandler`에 항상 존재하는 원본 `RE::SpellItem`과 그 `effects[i]`**로 연결하는 방식입니다.

- **사용할 클래스 및 함수**:
  - `CopySpellToEnchantment`에서 `enchantment->AddChange(1)` 삭제 `[CERTAIN]`.
  - `RE::MagicTarget::AddTarget(RE::MagicTarget::AddTargetData&)` 가상 함수 훅(`RE::VTABLE_PlayerCharacter[4]`, `RE::VTABLE_Character[4]`의 인덱스 `0x01` — 정확한 `VTABLE` 배열 인덱스는 로컬 헤더 대조 전까지 `미확인` `[GUESS]`, `RE::MagicTarget` 및 `GetActiveEffectList()`는 `[CERTAIN]`), 또는 시전 직후 `RE::MagicTarget::GetActiveEffectList()`를 순회하여 `RE::ActiveEffect::spell`(`RE::MagicItem*`)과 `RE::ActiveEffect::effect`(`RE::Effect*`)를 원본 `RE::SpellItem*` 및 `origSpell->effects[i]`로 교체 `[CERTAIN]`.
  - 단, 집중(Concentration) 주문(`a_spell->GetCastingType() == RE::MagicSystem::CastingType::kConcentration` `[CERTAIN]`)은 버튼을 누르는 동안 `ActorMagicCaster::currentSpell`과 `ActiveEffect::spell` 일치를 검사해 종료 처리할 수 있으므로 시전 중 재지정에서 제외합니다 `[GUESS — 종료 판정 내부 분기는 미확인]`. 지속형 버프·디버프·소환(`kFireAndForget`)은 발사 직후 `casterSpell`이 `00000000`이 되므로(`01:10:59.575` 로그) 안전합니다 `[CERTAIN]`.
  - 손 슬롯(`selectedSpells[kRightHand]`)은 코세이브 로드 시(`SKSE::SerializationInterface::SetLoadCallback` 또는 `kPostLoadGame`) 지팡이의 `(FormID + ExtraUniqueID)` 바인딩 정보로 새 복사본을 만들어 다시 채웁니다 `[CERTAIN]`.
- **SKSE 라이프사이클 시점**:
  - 효과 재지정: 런타임 시전 시점(`MagicTarget::AddTarget` 훅).
  - 손 슬롯 복구: 코세이브 `LoadCallback` 또는 `SKSE::MessagingInterface::kPostLoadGame` `[CERTAIN]`. (추가로 `kSaveGame`에서 손 슬롯을 지팡이 원래 `formEnchanting`으로 잠시 되돌리고 코세이브 `SaveCallback`에서 복구하면 `.ess`에 `0xFF` 참조가 단 하나도 남지 않습니다 `[GUESS — `kSaveGame`이 `.ess` 직렬화 직전에 호출되는 것은 첨부 로그 타임스탬프 `08.851` -> `09.258`과 일치하나 SKSE 버전에 따른 정확한 훅 위치는 확인 권장]`.)
- **비용**: `AddTarget` 호출 시 훅 1회와 짧은 포인터 매핑 조회뿐이라 런타임 및 세이브 용량 비용이 사실상 0입니다.
- **부작용 및 안전성 검토**:
  - **세이브 비대화 / 영구 잔류 폼**: `AddChange(1)`을 제거하므로 `.ess`에 남는 `0xFF` 폼이 0개입니다. 지팡이에 바인딩된 주문을 수십 번 바꿔도 이전 버프용 복사본을 코세이브에 보존할 필요가 없습니다.
  - **언인스톨 안전성 / 모드 설치 전 세이브**: 최상입니다. SKSE 플러그인을 완전히 지우고 세이브를 불러와도 Oakflesh(`0x0005AD5C`)는 바닐라 주문 효과로서 남은 지속시간 동안 정상 유지됩니다. 설치 전 세이브도 아무 영향이 없습니다.
  - **타 모드 호환성**: SkyUI 활성 효과 HUD에 빈 이름(`''`) 대신 실제 주문 이름(`참나무 피부`)과 아이콘이 표시되고, 손으로 같은 주문을 다시 외웠을 때 버프가 중첩(+80 방어력)되지 않고 정상 갱신됩니다.
  - **주의할 점**: `CopySpellToEnchantment`가 원본 `a_spell->effects`와 **1대1로 동일한 인덱스 순서**를 유지해야 하며, 현재 스파이크 코드(`CopySpellToEnchantment` 16~17행)처럼 `TESCondition` 깊은 복사를 생략한 상태에서는 원본 `origSpell->effects[i]`의 조건식(예: Mage Armor 퍽 조건)이 평가되어 퍽에 맞는 효과만 활성화됩니다 `[CERTAIN]`. 또한 바닥에 깔아 둔 룬(`Projectile`)처럼 아직 `ActiveEffect`가 되기 전 상태로 저장된 발사체는 이 방식만으로는 `0xFF`을 참조합니다 `[GUESS]`. (참고로 재지정 후에는 `effect->spell->IsDynamicForm()`이 `false`가 되므로 기존 `LogSaveState`의 `from runtime spells` 카운터 대신 `total` 및 원본 주문 FormID로 확인해야 합니다 `[CERTAIN]`.)

---

### 2순위: SKSE 코세이브 `LoadCallback`에서 `TESForm::SetFormID`로 `0xFF` 복사본을 선제 재생성
`AddChange(1)`을 제거한 뒤, 코세이브에 `(동적 0xFF FormID, 원본 SpellItem FormID)` 쌍을 저장하고 `.ess`가 액터와 `ActiveEffect`를 읽기 전에 똑같은 `0xFF` 번호로 복사본을 미리 만들어 두는 방식입니다(Dynamic Persistent Forms 계열 모드들이 사용하는 기법).

- **사용할 클래스 및 함수**:
  - `SKSE::SerializationInterface`(`SetUniqueID`, `SetSaveCallback`, `SetLoadCallback`, `SetRevertCallback`, `ResolveFormID`) `[CERTAIN]`.
  - `LoadCallback` 내부에서 `ResolveFormID`로 원본 `SpellItem` FormID를 현재 로드 오더에 맞게 변환 후 `RE::TESForm::LookupByID<RE::SpellItem>()` 조회 -> `CopySpellToEnchantment` 호출 -> **`enchantment->SetFormID(savedDynamicID, false)`**(`RE::TESForm::SetFormID(RE::FormID, bool)` `[CERTAIN]`) 호출로 `GetAllForms()` 해시맵 키를 저장 당시 `0xFF0039E6`으로 변경합니다.
- **SKSE 라이프사이클 시점**:
  - **반드시 코세이브 `LoadCallback`**에서 실행해야 합니다 `[CERTAIN]`. 첨부 로그에서 확인되듯 `LoadCallback`(`01:27:27.924`)은 `.ess` 헤더/플러그인 목록 직후, 액터 ChangeForm 및 `ActiveEffect` 로드보다 약 2.8초 먼저 실행되는 반면, `kPostLoadGame`(`01:27:30.730`)은 이미 7개 효과가 폐기된 뒤에 실행되므로 너무 늦습니다 `[CERTAIN]`. `kPreLoadGame`은 `.ess`의 플러그인 테이블을 읽기 전이라 `ResolveFormID`를 쓸 수 없어 부적합합니다 `[CERTAIN]`.
- **비용**: 살아 있는 복사본 테이블 관리 및 코세이브 직렬화 비용이 소량 발생합니다.
- **부작용 및 안전성 검토**:
  - **장점**: `ActiveEffect`뿐 아니라 손 슬롯(`slot=FF0039E6`)과 공중의 발사체·설치된 룬까지 모두 `0xFF0039E6`을 그대로 찾아 복원됩니다.
  - **깨질 수 있는 지점**:
    1. 지팡이 주문을 A에서 B로 바꿨는데 플레이어나 멀리 떨어진 NPC에게 아직 주문 A의 `ActiveEffect`가 남아 있을 때 세이브하면, 단순히 현재 지팡이에 묶인 주문만 저장해서는 A의 `0xFF` ID가 누락되어 효과가 증발합니다(참조 추적/가비지 컬렉션 필요).
    2. 같은 프로세스에서 재로드할 때 `RevertCallback`에서 기존 `EnchantmentItem`을 성급히 `delete`하면 엔진이 이전 세이브의 `ActiveEffect`를 정리하는 도중 Use-After-Free CTD가 날 수 있고, 반대로 방치하면 과거 세이브 로드 시 되감긴 동적 FormID 카운터와 충돌할 수 있습니다 `[GUESS]`. 따라서 이미 `LookupByID`에 같은 복사본이 있으면 제자리 재사용하고 미사용 복사본 정리는 `kPostLoadGame` 이후로 미뤄야 합니다.
    3. 모드를 언인스톨하면 첫 로드 시 해당 버프들은 사라집니다(`AddChange(1)`을 뺐다면 영구 찌꺼기는 남지 않습니다).

---

### 3순위: `0xFF` 복사본을 슬롯에 넣지 않고 지팡이 공격 시 원본 `SpellItem`을 `MagicCaster`로 직접 발사
Ordinator(Spellscribe)나 Conduit 계열 무기 주문 모드에서 흔히 쓰는 방식으로, 지팡이 우클릭/발사 시점에 별도 캐스터(`RE::Actor::GetMagicCaster(RE::MagicSystem::CastingSource::kOther)` 및 `RE::MagicCaster::CastSpellImmediate` 등 `[CERTAIN]`)로 원본 `SpellItem`을 직접 쏘게 만듭니다.

- **사용할 클래스 및 함수**: `RE::Actor::GetMagicCaster`, `RE::MagicCaster::CastSpellImmediate` (또는 `ActorMagicCaster` 가상 함수 훅) `[CERTAIN]`.
- **SKSE 라이프사이클 시점**: 런타임 전투/입력 이벤트 시점(세이브·로드 복원 로직 자체가 불필요, 코세이브에는 지팡이별 선택 주문만 저장).
- **비용 및 한계**: 동적 `0xFF` 폼을 아예 만들지 않으므로 세이브 오염이 0이고 언인스톨에 완벽히 안전합니다. 다만 지팡이의 충전 시간(`chargeTime`), 우클릭 유지형 집중 주문 애니메이션, 조준선 및 매지카 소모 타이밍을 지팡이 공격 상태 머신과 수동으로 맞춰야 해서 현재 아키텍처를 크게 고쳐야 합니다.

---

### 4순위: 보조 `.esl` 플러그인에 정적 `EnchantmentItem` 슬롯 풀(`0xFE` FormID)을 두고 `LoadCallback`에서 채우기
동적 `0xFF` 생성 대신 `.esl` 파일에 미리 빈 `EnchantmentItem` 레코드 N개를 만들어 두고 런타임에 `effects`와 `data`만 덮어쓰는 방식입니다.

- **사용할 클래스 및 함수**: `RE::TESDataHandler::LookupForm<RE::EnchantmentItem>(localID, "StaffPool.esl")` `[CERTAIN]`, 코세이브 `LoadCallback`에서 `effects` 및 `data` 재구성 `[CERTAIN]`.
- **SKSE 라이프사이클 시점**: **반드시 코세이브 `LoadCallback`** (`kPostLoadGame`에서 채우면 첨부 로그의 `POOL refilled`처럼 `.ess` 로드 시점에 `effects`가 비어 있어 효과가 사라집니다 `[CERTAIN]`).
- **비용 및 한계**: `0xFF` 충돌 위험은 없지만 단독 DLL(Standalone) 요건이 깨지며, 슬롯 개수가 고정되어 여러 종류의 지속 주문을 연속 시전하면 슬롯이 고갈되거나 이전 효과를 덮어쓰고, `.esl` 제거 시 세이브 호환성이 떨어집니다.

---

### 5순위 (최하위): 마법부여대 메커니즘(`RE::BGSCreatedObjectManager`) 사용 + 코세이브 데이터 보정
엔진이 플레이어 제작 마법부여를 저장하는 `BGSCreatedObjectManager`에 복사본을 등록하여 `.ess`의 Global Data Type 6에 `effects` 배열이 함께 저장되게 만드는 방식입니다.

- **사용할 클래스 및 함수**: `RE::BGSCreatedObjectManager::GetSingleton()->AddWeaponEnchantment(BSTArray<RE::Effect>&)` (방어구용은 `AddArmorEnchantment`, 해제용은 `DestroyEnchantment` — `DestroyEnchantment`의 정확한 매개변수 시그니처는 헤더 확인 필요 `미확인` `[GUESS]`, 클래스와 `AddWeaponEnchantment`/`AddArmorEnchantment` 존재는 `[CERTAIN]`).
- **SKSE 라이프사이클 시점**: 시전/바인딩 시 생성, 코세이브 `LoadCallback` 또는 `kPostLoadGame`에서 `EnchantmentItem::Data` 재패치.
- **왜 최하위인가 (치명적 한계)**:
  1. `BGSCreatedObjectManager`는 무기 마법부여(`kEnchantment` + `kFireAndForget` + `kTouch`)와 방어구 마법부여(`kEnchantment` + `kConstantEffect` + `kSelf`) 두 테이블만 관리하며, 지팡이용 `SpellType::kStaffEnchantment`, `chargeTime`, 원거리/자신 `Delivery`, `kConcentration`, 그리고 복잡한 `TESCondition` 체인을 그대로 보존하지 못합니다 `[GUESS — Global Data Type 6 직렬화 필드는 바이너리 포맷 영역이라 미확인이나 무기/방어구 전용 설계상 명확한 한계]`. 결국 코세이브 로더에서 `data`와 조건식을 다시 패치해야 합니다.
  2. 아이템에 `ExtraEnchantment`를 붙이지 않는 구조(`onItem=false`)에서는 엔진의 참조 카운트 정리가 동작하지 않아 `.ess` 내부 Global Data에 영구적으로 `0xFF` 마법부여가 쌓이며(세이브 비대화), 모드를 지우면 깨진 무기 마법부여가 손 슬롯에 남습니다 `[GUESS]`.

---

## 3. 1순위 해결책 C++ 스케치 및 로그 기반 자동 판정(PASS/FAIL) 셀프테스트

### 핵심 변경 사항 요약
1. `CopySpellToEnchantment`에서 **`enchantment->AddChange(1)`을 제거**하고, 복사본 `EnchantmentItem*` -> 원본 `SpellItem*` 매핑을 런타임 맵에 기록합니다.
2. `MagicTarget::AddTarget` 훅(비집중 주문)에서 `AddTargetData`의 `magicItem`과 `effect`를 원본 `SpellItem`과 `origSpell->effects[i]`로 교체하여, 생성되는 모든 `ActiveEffect`가 처음부터 정적 원본 주문을 가리키게 합니다. (`AddTargetData` 멤버명 및 `VTABLE_PlayerCharacter[4]` 인덱스는 빌드 시 로컬 CommonLibSSE-NG `RE/M/MagicTarget.h` 헤더와 대조가 필요합니다: `미확인` `[GUESS]`.)
3. 셀프테스트는 세이브 시점의 **OS 프로세스 ID(`GetCurrentProcessId()`)**와 기대 효과 수·주문 ID를 코세이브에 함께 기록하여, 단순 같은 프로세스 재로드가 아니라 **실제 게임 재시작(`crossProcess=true`)** 후에도 효과 개수·핸드 슬롯 상태·동적 폼 잔류 여부가 모두 통과했는지를 로그 한 줄(`SELFTEST PASS` / `SELFTEST FAIL`)로 판정합니다.

### C++ 스케치 (CommonLibSSE-NG)

```cpp
#include <Windows.h>
#include <mutex>
#include <unordered_map>

namespace StaffPersistenceFix
{
	// 런타임 0xFF 복사본 -> 원본 정적 SpellItem 매핑
	std::mutex g_copyMutex;
	std::unordered_map<const RE::MagicItem*, RE::SpellItem*> g_copyToOriginal;

	void RegisterRuntimeCopy(const RE::EnchantmentItem* a_copy, RE::SpellItem* a_original)
	{
		if (!a_copy || !a_original) {
			return;
		}
		std::scoped_lock lk(g_copyMutex);
		g_copyToOriginal[a_copy] = a_original;
	}

	RE::SpellItem* LookupOriginalSpell(const RE::MagicItem* a_copy)
	{
		if (!a_copy || !a_copy->IsDynamicForm()) {
			return nullptr;
		}
		std::scoped_lock lk(g_copyMutex);
		const auto it = g_copyToOriginal.find(a_copy);
		return it != g_copyToOriginal.end() ? it->second : nullptr;
	}

	// CopySpellToEnchantment 수정 포인트:
	// 1) enchantment->AddChange(1); 줄을 반드시 삭제합니다 (빈 껍데기 0xFF ChangeForm 방지).
	// 2) a_spell->effects의 모든 원소를 건너뛰지 않고 1:1 동일 인덱스로 복사합니다 (TESCondition 깊은 복사 포함).
	// 3) 반환 직전 RegisterRuntimeCopy(enchantment, a_spell); 를 호출합니다.

	// MagicTarget::AddTarget 훅: 비집중 주문 효과를 원본 SpellItem으로 재지정
	// 주의: VTABLE 슬롯 번호([4]의 0x01)와 AddTargetData 필드명(magicItem, effect)은
	// 로컬 CommonLibSSE-NG의 RE/M/MagicTarget.h 헤더에서 대조 후 컴파일하세요 (미확인 [GUESS]).
	template <class TargetActorVTable>
	struct AddTargetHook
	{
		static bool thunk(RE::MagicTarget* a_target, RE::MagicTarget::AddTargetData& a_data)
		{
			if (auto* origSpell = LookupOriginalSpell(a_data.magicItem)) {
				if (origSpell->GetCastingType() != RE::MagicSystem::CastingType::kConcentration && a_data.effect) {
					const auto& copyEffects = a_data.magicItem->effects;
					const auto& origEffects = origSpell->effects;
					for (std::uint32_t i = 0; i < copyEffects.size() && i < origEffects.size(); ++i) {
						if (copyEffects[i] == a_data.effect) {
							a_data.magicItem = origSpell;
							a_data.effect = origEffects[i];
							break;
						}
					}
				}
			}
			return func(a_target, a_data);
		}

		static inline REL::Relocation<decltype(thunk)> func;

		static void Install(REL::VariantID a_vtable)
		{
			REL::Relocation<std::uintptr_t> vtbl{ a_vtable };
			func = vtbl.write_vfunc(0x01, thunk);
		}
	};

	void InstallHooks()
	{
		AddTargetHook<RE::PlayerCharacter>::Install(RE::VTABLE_PlayerCharacter[4]);
		AddTargetHook<RE::Character>::Install(RE::VTABLE_Character[4]);
	}

	// ---------------- 자동 검증 셀프테스트 (코세이브 기록 + kPostLoadGame 판정) ----------------
	constexpr std::uint32_t kSelfTestRecordType = 'TSAV';
	constexpr std::uint32_t kSelfTestRecordVer = 1;

	struct SelfTestExpectation
	{
		bool          armed{ false };
		std::uint32_t savedPid{ 0 };
		RE::FormID    expectedSpellId{ 0 };
		std::uint32_t expectedEffectCount{ 0 };
	} g_selfTest;

	std::pair<std::uint32_t, std::uint32_t> CountPlayerEffects(RE::FormID a_targetSpellId)
	{
		std::uint32_t matchingStatic = 0;
		std::uint32_t lingeringDynamic = 0;
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* target = player ? player->AsMagicTarget() : nullptr;
		auto* effects = target ? target->GetActiveEffectList() : nullptr;
		if (!effects) {
			return { 0, 0 };
		}
		for (const auto* ae : *effects) {
			if (!ae || !ae->spell) {
				continue;
			}
			if (ae->spell->IsDynamicForm()) {
				++lingeringDynamic;
			}
			if (ae->spell->GetFormID() == a_targetSpellId && ae->duration > 0.0f && ae->elapsedSeconds < ae->duration) {
				++matchingStatic;
			}
		}
		return { matchingStatic, lingeringDynamic };
	}

	// SKSE SaveCallback 내부에서 호출 (예: Oakflesh 0x0005AD5C 테스트 중일 때 기록)
	void SaveSelfTest(SKSE::SerializationInterface* a_intfc, RE::FormID a_testedSpellId)
	{
		const auto [matching, dynamicCount] = CountPlayerEffects(a_testedSpellId);
		if (!a_intfc->OpenRecord(kSelfTestRecordType, kSelfTestRecordVer)) {
			return;
		}
		const std::uint32_t pid = ::GetCurrentProcessId();
		a_intfc->WriteRecordData(&pid, sizeof(pid));
		a_intfc->WriteRecordData(&a_testedSpellId, sizeof(a_testedSpellId));
		a_intfc->WriteRecordData(&matching, sizeof(matching));
		logger::info("SELFTEST ARMED T-SAVE-EFFECTS pid={} spell={:08X} savedMatching={} lingeringDynamic={}",
			pid, a_testedSpellId, matching, dynamicCount);
	}

	// SKSE RevertCallback 내부에서 호출
	void RevertSelfTest()
	{
		g_selfTest = {};
	}

	// SKSE LoadCallback 내부에서 호출
	void LoadSelfTest(SKSE::SerializationInterface* a_intfc)
	{
		std::uint32_t pid = 0;
		RE::FormID    rawSpellId = 0;
		std::uint32_t expectedCount = 0;
		if (!a_intfc->ReadRecordData(&pid, sizeof(pid)) ||
			!a_intfc->ReadRecordData(&rawSpellId, sizeof(rawSpellId)) ||
			!a_intfc->ReadRecordData(&expectedCount, sizeof(expectedCount))) {
			return;
		}
		RE::FormID resolvedSpellId = 0;
		if (rawSpellId != 0 && !a_intfc->ResolveFormID(rawSpellId, resolvedSpellId)) {
			resolvedSpellId = 0;
		}
		g_selfTest = { true, pid, resolvedSpellId, expectedCount };
	}

	// SKSE kPostLoadGame 메시지 핸들러(바인딩된 지팡이 복사본 재장착 직후)에서 호출
	void EvaluateSelfTestPostLoad()
	{
		if (!g_selfTest.armed || g_selfTest.expectedEffectCount == 0) {
			return;
		}
		const bool crossProcess = (::GetCurrentProcessId() != g_selfTest.savedPid);
		const auto [actualMatching, dynamicEffects] = CountPlayerEffects(g_selfTest.expectedSpellId);

		auto* player = RE::PlayerCharacter::GetSingleton();
		const auto* slot = player ? player->GetActorRuntimeData().selectedSpells[RE::Actor::SlotTypes::kRightHand] : nullptr;
		// 슬롯이 비어 있거나, 채워져 있다면 반드시 effects가 비어 있지 않은 유효 마법이어야 함 (빈 0xFF 껍데기 검출)
		const bool slotHealthy = (!slot) || (!slot->effects.empty());

		const bool pass = crossProcess &&
		                  (g_selfTest.expectedSpellId != 0) &&
		                  (actualMatching == g_selfTest.expectedEffectCount) &&
		                  (dynamicEffects == 0) &&
		                  slotHealthy;

		logger::info("SELFTEST {} T-SAVE-EFFECTS crossProcess={} spell={:08X} expected={} actual={} dynamicEffects={} slot={:08X} slotEffects={}",
			pass ? "PASS" : "FAIL",
			crossProcess,
			g_selfTest.expectedSpellId,
			g_selfTest.expectedEffectCount,
			actualMatching,
			dynamicEffects,
			slot ? slot->GetFormID() : 0u,
			slot ? slot->effects.size() : 0u);
	}
}
```

### 셀프테스트 로그 판정 기준
- 같은 프로세스 안에서만 재로드한 경우 `crossProcess=false`로 찍히므로 `SELFTEST FAIL` 처리되어 착시 통과를 막습니다.
- 게임을 완전히 종료한 뒤 재시작해 세이브를 불러왔을 때(`crossProcess=true`), 저장 시점의 버프 효과 수와 로드 후 효과 수가 정확히 일치하고(`expected == actual`), `0xFF` 동적 주문을 가리키는 잔류 효과가 `0`개이며(`dynamicEffects=0`), 오른손 슬롯에 `effects`가 0개인 빈 껍데기 폼이 남아 있지 않을 때만 `SELFTEST PASS T-SAVE-EFFECTS`가 기록됩니다.
