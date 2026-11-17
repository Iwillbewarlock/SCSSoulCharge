첨부된 코드(`testcontrols.cpp`, `staff_art.cpp`)와 `2026-11-10` 실행 로그, 그리고 Skyrim AE(1.6.1170) 엔진 및 CommonLibSSE-NG 지식을 바탕으로 진단한 결과입니다. 확인하지 못한 CommonLibSSE-NG 헤더의 정확한 멤버·가상함수 식별자는 **`(미확인)`**으로 병기했습니다.

---

### 사전 정리: 엔진이 충전 중 지팡이 팁 아트를 다루는 구조

1. **렌더링 주체 객체**: 지팡이 충전 팁 아트는 `BSTempEffect` / `ModelReferenceEffect` / `ShaderReferenceEffect`가 아닙니다 **`[확실 / CERTAIN: RE::ModelReferenceEffect, RE::ShaderReferenceEffect는 RE::BGSReferenceEffect 및 무기 상시 인챈트 비주얼용 RE::WeaponEnchantmentController에서 사용됨]`**. 시전 아트(`RE::BGSArtObject`)는 `RE::ActorMagicCaster` 자체가 `RE::SimpleAnimationGraphManagerHolder`(`+0x48`)와 `RE::BSTEventSink<RE::BSAnimationGraphEvent>`(`+0x60`)를 직접 상속하여 관리하며 **`[확실 / CERTAIN: RE::ActorMagicCaster 상속 구조]`**, 내부의 부착 데이터(`+0x68`, 타입 `RE::RefAttachTechniqueInput`, 멤버명 `castingArtData` 또는 `unk68` **`[추정 / GUESS, 미확인]`**)와 비동기 복제 작업(`+0xB0`, 타입 `RE::NiPointer<RE::BGSArtObjectCloneTask>`, 멤버명 `cloneTask` 또는 `unkB0` **`[추정 / GUESS, 미확인]`**)을 통해 `RE::ActorMagicCaster::magicNode`(`+0xC0`)에 NIF 3D를 붙입니다 **`[확실 / CERTAIN: RE::ActorMagicCaster::magicNode, RE::MagicCaster::GetMagicNode]`**.
2. **`castingArt` 포인터를 읽고 쓰는 시점**: `RE::ActorMagicCaster::castingArt`(`+0xE0`) **`[확실 / CERTAIN: testcontrols.cpp:L12 caster->castingArt]`**는 무기 장착 및 주문 선택 시점(`SelectSpellImpl`, `MagicCaster` vtable `0x10` **`[추정 / GUESS, 미확인]`**)에만 MGEF에서 읽어 캐시합니다 **`[추정 / GUESS]`**. 충전·시전 단계(`state` `1 -> 2 -> 3 -> 4 -> 0` 및 `1 -> 2 -> 4 -> 6 -> 0`) 중에는 엔진이 MGEF나 지팡이 레코드에서 `caster->castingArt`를 다시 덮어쓰지 않습니다 **`[확실 / CERTAIN: 로그:L6-L10, L25-L42, L47-L52에서 전 상태 내내 castArt=000334B9 유지]`**.

---

### 1. 가능한 원인 목록 (가능성 순)

#### 1순위: 대기(`state == 0`) 중 원시 포인터(`caster->castingArt`)만 덮어써서 기존 부착 3D·애니메이션 그래프가 해제되지 않는 상태 불일치
- **원인 명제**: **만약** 지팡이를 장착한 채 `slot = enchantment`(`testcontrols.cpp(step 발췌):L4`)와 `caster->castingArt = a_art`(`testcontrols.cpp:L12`)만 직접 대입하고 `DeselectSpellImpl` / `SelectSpellImpl`을 호출하지 않으면, **엔진이 기존에 생성된 아트 3D 또는 `SimpleAnimationGraphManagerHolder` 인스턴스를 정리하지 않은 채 재사용하거나 특정 리셋 조건에서만 재부착하기 때문에**, **새 아트가 어떤 충전에서는 보이지 않고 어떤 충전에서는 보이는("없다가 있다가") 현상이 발생합니다.**
- **근거**:
  - 바닐라 엔진에서 `RE::ActorMagicCaster::castingArt`가 바뀌는 정상 경로는 기존 주문을 해제(`DeselectSpellImpl`, vtable `0x11` **`[추정 / GUESS, 미확인]`**)하여 기존 아트 3D·애니메이션 그래프를 내린 뒤 새 주문을 선택(`SelectSpellImpl` **`[추정 / GUESS, 미확인]`**)하는 것뿐입니다 **`[추정 / GUESS]`**.
  - `testcontrols.cpp:L9-L16` 및 `step 발췌:L4-L8`은 `selectedSpells[kRightHand]`와 `caster->castingArt` 포인터만 바꿉니다 **`[확실 / CERTAIN: 첨부 코드]`**. 또한 `staff_art.cpp:L60`에는 있던 `caster->state.underlying() == 0` 가드도 `SetCastingArt`(`testcontrols.cpp:L11`)에는 없습니다 **`[확실 / CERTAIN: 첨부 코드 비교]`**.
  - `ActorMagicCaster`의 아트 부착 함수(`CheckAttachCastingArt`, vtable `0x1B` **`[추정 / GUESS, 미확인]`**)가 "현재 부착된 3D/그래프 포인터가 비어 있는가"만 검사하고 "부착된 3D의 원본 `BGSArtObject*`가 현재 `caster->castingArt`와 같은가"를 매번 대조하지 않는다면 **`[추정 / GUESS]`**, 이전 단계(초기 장착의 `0001B211` 또는 직전 시전 단계)의 3D/애니메이션 그래프가 종료·비가시 상태로 남거나 시전 유형(`Flames` 집중형 `state 6 -> 0` 후 Step 3 진입 등) 전환 시 그래프 이벤트가 어긋나 팁 이펙트가 누락됩니다 **`[추정 / GUESS]`**. 실제로 `로그:L25-L42`(Step 2 `Flames` 직후인 Step 3 `Ice Spike`)에서만 테스터가 유일하게 **3회 연속 시전**(`08.9초`, `12.4초`, `19.6초`)을 반복했습니다 **`[확실 / CERTAIN: 로그:L25-L42]`**.
  - **지팡이 고유 아트(`0001B211`)만 멀쩡한 이유**: 초기 장착 시점에 이미 `0001B211` 기준으로 3D와 애니메이션 그래프가 초기화되어 있으므로, 기존 인스턴스가 그대로 재사용되더라도 대상 아트가 동일하여 불일치가 드러나지 않습니다 **`[추정 / GUESS]`**. (이전 `staff_art.cpp:L107-L118`은 MagicMenu에서 `ActorEquipManager::EquipSpell` 훅 직후 동작했고 단일 바인딩 위주로 쓰여 단계별 즉시 교체와 조건이 달랐습니다 **`[확실 / CERTAIN: staff_art.cpp:L107-L118]`**.)

#### 2순위: 시전 시작(`state 1 -> 2`) 시 비동기 NIF 복제·스트리밍(`BGSArtObjectCloneTask`)과 단발성 충전 애니메이션 이벤트 간의 경합
- **원인 명제**: **만약** 지팡이 캐스터가 대기 중에는 아트를 떼어 두었다가 시전 시작(`state = 1` 또는 `2`) 시점에 `BGSArtObjectCloneTask`로 비동기 로드·복제를 요청한다면, **사전 로드되지 않은 외부 아트의 NIF 복제가 완료되기 전에 충전 시작 애니메이션 이벤트가 먼저 발송되어 유실되기 때문에**, **한 번의 충전 안에서 늦게 켜지거나 첫 시전에서 누락되는 현상이 발생합니다.**
- **근거**:
  - `RE::BGSArtObject`는 `RE::TESModelTextureSwap`을 통해 NIF 경로만 보유하며 **`[확실 / CERTAIN: RE::BGSArtObject 상속 구조]`**, `ActorMagicCaster`는 백그라운드 복제 태스크(`BGSArtObjectCloneTask`, `+0xB0` **`[추정 / GUESS, 미확인]`**)로 3D를 복제한 뒤 `RE::SimpleAnimationGraphManagerHolder::NotifyAnimationGraph`로 충전/방출 애니메이션을 구동합니다 **`[확실 / CERTAIN: RE::SimpleAnimationGraphManagerHolder::NotifyAnimationGraph]`** **`[추정 / GUESS: 복제 완료 시점 동작]`**.
  - `testcontrols.cpp(step 발췌):L4`는 런타임 `0xFF` 인챈트만 슬롯에 넣을 뿐 해당 `SpellItem`을 실제 장착(`EquipSpell`)하지 않으므로(`staff_art.cpp:L107`의 메뉴 장착이나 `L141`의 발사 후 학습과 다름), `000334B9`(`IceSpikeHandFX01.nif`)와 `00056AC7`(`AlterationHandFX01.nif`)이 `BSModelDB`에 사전 로드되어 있지 않거나 시전 종료(`FinishCastImpl`) 시 참조 카운트가 0으로 내려가 해제될 수 있습니다 **`[추정 / GUESS]`**.
  - 로그상 `state=1`에서 `state=2`(충전 시작)까지의 간격은 **29~41ms(약 2프레임)**에 불과하며(`로그:L6-L7` 29ms, `L25-L27` 39ms, `L47-L49` 37ms), 단발형 주문의 충전 시간(`chargeTime=0.50`, `로그:L3, L23, L69`)도 0.5초뿐입니다 **`[확실 / CERTAIN: 첨부 로그 타임스탬프]`**. 복제가 `state=2` 진입 직후의 단발성 그래프 이벤트보다 늦게 끝나면 충전 내내 안 보이다가 `state=3`(완충 대기)에서야 나타나거나 아예 빠집니다 **`[추정 / GUESS]`**.
  - **지팡이 고유 아트(`0001B211`)만 멀쩡한 이유**: 지팡이(`0004C6CA` -> `0004C6CB` -> `00012F03`) 장착 시 `0001B211` 모델이 이미 메모리에 올라와 있었고 **`[추정 / GUESS]`**, 결정적으로 로그의 모든 `0001B211` 단계(Step 2·5)는 7~9.6초 동안 지속 루프를 도는 집중형(`Flames`)이었습니다 **`[확실 / CERTAIN: 로그:L13-L20, L55-L62]`**.

#### 3순위: 실험 단계의 시전 방식(`castType`) 및 잔여 충전량(`charge=0.00`) 교란 요인
- **원인 명제**: **만약** 런타임 생성 지팡이 인챈트에서 단발형(`castType=1`, `state 1->2->3->4->0`)의 완충 유지 구간(`state=3`)이나 충전량 고갈(`charge=0.00`, Step 6·7) 처리 경로가 아트/시전 상태를 다르게 다룬다면, **이 스파이크에서 `0001B211`은 집중형(`castType=2`, `charge > 0`)으로만 시험되었고 다른 아트(`000334B9`, `00056AC7`)는 전부 단발형(`castType=1`) 및 `charge=0.00` 조건을 포함해 시험되었기 때문에**, **지팡이 고유 아트 이외의 아트만 고장 난 것처럼 관찰됩니다.**
- **근거**:
  - `로그:L1-L71` 전체에서 `0001B211`은 Step 2·5의 `Flames`(`castType=2`, `chargeTime=0.00`, `state 1->2->4->6->0`, `state=3` 미진입)뿐이며, `0001B211`을 쓰는 단발형 주문(예: 바닐라 `Firebolt`)은 한 번도 시험되지 않았습니다 **`[확실 / CERTAIN: 로그:L13, L55]`**. 반대로 `000334B9`(Step 1·3·4·6)와 `00056AC7`(Step 7, 게다가 지팡이에는 비표준인 `self buff` 자가 타겟)은 모두 `castType=1`, `chargeTime=0.50`입니다 **`[확실 / CERTAIN: 로그:L3, L23, L45, L65, L69]`**.
  - 특히 Step 6(`로그:L65`, `charge=0.00`)에서는 시전 로그가 없고, Step 7(`로그:L69-L71`, `Oakflesh`, `charge=0.00`)에서는 `01:10:55.718`에 공격 버튼을 눌렀음에도 앞선 Step 1~5(0~28ms 내 `state=1` 진입)와 달리 `caster state=1` 로그가 뒤따르지 않습니다 **`[확실 / CERTAIN: 로그:L63-L71]`**. 만약 로그 발췌가 잘린 것이 아니라 `kRightItemCharge == 0.00` 때문에 `CheckCast`/`RequestCastImpl` 단계에서 시전 진입이 막히거나 지연된 것이라면 **`[추정 / GUESS]`**, 해당 단계에서 팁 아트가 아예 뜨지 않습니다.

#### 4순위: 런타임 `EnchantmentItem`의 다중 효과 `GetCostliestEffectItem()` 불일치 및 재선택 시점 참조
- **원인 명제**: **만약** 엔진이 시전 보조 처리나 장착 갱신(`onItem` 전환 등) 중 `caster->currentSpell`(또는 `selectedSpells[kRightHand]`)에서 `GetCostliestEffectItem()`을 호출해 MGEF 아트를 참조하는데 런타임 `EnchantmentItem`의 효과별 비용(`RE::Effect::cost`)이 원본 `SpellItem`과 다르게 계산·복사되었다면, **부가 효과가 6개·8개인 `Ice Spike`와 `Oakflesh`에서 `castingArt == nullptr`인 보조 MGEF가 선택되기 때문에**, **해당 주문들에서만 아트가 누락될 수 있습니다.**
- **근거**:
  - `testcontrols.cpp(step 발췌):L5`는 슬롯에 넣은 `enchantment`가 아니라 원본 `spell->GetCostliestEffectItem()`에서 `artSource`를 구합니다 **`[확실 / CERTAIN: step 발췌:L4-L6]`**.
  - `RE::MagicItem::GetCostliestEffectItem`은 `effects` 배열의 `RE::Effect::cost` 값을 비교해 최고 비용 항목을 반환합니다 **`[확실 / CERTAIN: RE::MagicItem::GetCostliestEffectItem, RE::Effect::cost]`**. `Flames`는 `effects=2`(`로그:L13`)인 반면 `Ice Spike`는 `effects=6`(`로그:L3`), `Oakflesh`는 `effects=8`(`로그:L69`)이며, 스카이림 다중 효과 주문의 부가 효과(둔화·퍽 스크립트 등)는 `data.castingArt == nullptr`인 경우가 흔합니다 **`[확실 / CERTAIN: 로그 효과 개수 및 바닐라 MGEF 데이터 구조]`**. 단, `로그:L6-L52`에서 `caster->castingArt` 자체는 `00000000`으로 덮어써지지 않았으므로 **`[확실 / CERTAIN: 첨부 로그]`**, 엔진이 `caster->castingArt` 대신 `currentSpell`의 MGEF를 직접 읽는 보조 경로가 있을 때만 성립하여 순위를 낮게 두었습니다 **`[추정 / GUESS]`**.

#### 5순위: `ExtraDataList` 변경(`onItem` 전환)·시점 전환에 따른 `magicNode` 캐시 무효화 누락 및 공유 MGEF `00012F03` 수정 부작용
- **원인 명제**: **만약** 아이템의 `ExtraEnchantment` 유무를 바꾸는 과정(`onItem=true` -> `onItem=false`, `로그:L23, L45`)이나 1·3인칭 전환 시 지팡이 3D `NiNode`가 재생성되는데 `ClearMagicNode()`가 호출되지 않으면, **`caster->magicNode`(`+0xC0`)가 분리된 구 3D 트리를 가리키기 때문에**, **새로 복제된 아트가 화면에 보이지 않는 노드에 붙게 됩니다.**
- **근거 및 공유 MGEF `00012F03` 상호작용**:
  - `RE::ActorMagicCaster::magicNode`는 지팡이 NIF의 `"MagicNode"` 포인터를 캐시하며 `RE::MagicCaster::ClearMagicNode`(vtable `0x0E` **`[확실 / CERTAIN: RE::MagicCaster::ClearMagicNode]`**) 전까지 유지됩니다 **`[추정 / GUESS: 재생성 시 무효화 타이밍]`**.
  - 한편 `SetRecordArt`(`testcontrols.cpp:L30-L47`)가 수정하는 `00012F03`(`RE::EffectSetting`)은 전역 싱글턴 폼입니다 **`[확실 / CERTAIN: RE::EffectSetting]`**. 엔진은 런타임에 `EffectSetting::data.castingArt`를 스스로 되돌리지 않으므로 **`[확실 / CERTAIN]`**, 공유 MGEF 수정 자체가 플레이어의 팁 아트를 깜빡이게 만들지는 않습니다(단, `RestoreRecordArt`가 프레임 중 호출되지 않는 한). 대신 같은 `00012F03`을 쓰는 모든 NPC의 지팡이가 플레이어가 바인딩한 아트를 함께 출력하는 전역 오염 부작용을 일으킵니다 **`[확실 / CERTAIN: testcontrols.cpp:L29 주석 및 엔진 구조]`**.

---

### 2. 원인별 최소 비용 판별법 (로그 우선)

모든 항목은 `caster->state` 전이 로그를 찍는 지점(프레임/상태 감시 루프)에 필드 몇 개만 추가하여 **로그만으로 판정**할 수 있습니다.

1. **1순위(기존 3D·애니메이션 그래프 미해제) 판별**:
   - **로그 시점 및 필드**: `SetCastingArt` 직전/직후(`state == 0`)와 각 `state` 전이(`0, 1, 2, 3, 4, 6`)마다 다음 값을 기록합니다.
     - `caster->GetMagicNode()`(또는 `caster->magicNode`) 포인터 주소 및 자식 노드 개수·이름: `magicNode ? magicNode->GetChildren().size() : -1` 및 각 `child->name.c_str()` **`[확실 / CERTAIN: RE::NiNode::GetChildren, RE::NiObjectNET::name]`**.
     - `RE::BSAnimationGraphManagerPtr graphMgr; caster->GetAnimationGraphManager(graphMgr);` 호출 성공 여부와 `graphMgr.get()` 주소 **`[확실 / CERTAIN: RE::IAnimationGraphManagerHolder::GetAnimationGraphManager]`**.
   - **로그 판정 기준**: `SetCastingArt`로 아트를 바꾼 직후(`state == 0`) 또는 다음 시전 `state == 2`에서 `magicNode` 자식 이름이나 `graphMgr` 포인터 주소가 **이전 단계와 동일한 주소로 유지**되거나, Step 3의 1회차와 2회차 시전 사이에 주소/자식 구성이 달라지면 1순위가 확정됩니다. (불가피한 육안 확인: 한 번의 충전 안에서 깜빡이는지 vs 충전 회차마다 통째로 있고 없는지만 테스터가 O/X로 표시하면 됩니다.)

2. **2순위(비동기 `BGSArtObjectCloneTask` 지연 경합) 판별**:
   - **로그 시점 및 필드**: `state == 1` 진입 프레임부터 `state == 3` 진입 프레임까지 매 프레임(최대 30프레임) 동안 `magicNode->GetChildren().size()`, `graphMgr.get() != nullptr`, 그리고 `ActorMagicCaster + 0xB0` 포인터(`cloneTask` 멤버명 **`(미확인)`** **`[추정 / GUESS]`** — 헤더에 멤버명이 없으면 `magicNode` 자식 수와 `graphMgr`만으로도 충분)를 기록합니다.
   - **로그 판정 기준**: `state == 2` 진입 프레임에는 `magicNode` 자식이 `0`개(`graphMgr == nullptr`)였다가, **수십~수백 ms 뒤 같은 `state == 2` 후반부나 `state == 3` 프레임에서 자식 노드가 뒤늦게 추가**되면 2순위가 확정됩니다.

3. **3순위(`castType` 교란 및 `charge=0.00` 차단) 판별**:
   - **로그 및 스파이크 단계 추가**: 스파이크 테이블에 ① `Firebolt`(`00012FD0`, `castType=1`, `chargeTime=0.50`, 아트 `0001B211`, `charge > 0`) 단계와 ② `Frostbite`(`0002B96B`, `castType=2`, `chargeTime=0.00`, 냉기 아트 `00020BF5`, `charge > 0`) 단계를 추가하고, 공격 입력 시 `RE::MagicSystem::CannotCastReason reason{}; caster->CheckCast(enchantment, false, nullptr, &reason, false)` 반환값과 `reason`을 함께 찍습니다 **`[확실 / CERTAIN: RE::MagicCaster::CheckCast, RE::MagicSystem::CannotCastReason]`**.
   - **로그/관찰 판정 기준**: Step 7(`charge=0.00`)에서 `CheckCast`가 `false`(예: `kNotEnoughCharge`)를 반환해 `state=1`로 못 들어가면 Step 6·7 누락 원인이 즉시 분리되며, ① `Firebolt`에서도 똑같이 누락되면 "고유 아트여서 안 깨졌다"는 가설이 기각되고 `castType=1` 경로 문제로 확정됩니다(이 항목만 ①·②의 팁 유무를 테스터가 육안 확인해야 합니다).

4. **4순위(다중 효과 `GetCostliestEffectItem` 불일치) 판별**:
   - **로그 시점 및 필드**: `slot = enchantment`(`step 발췌:L4`) 직후 한 줄로 `spell->GetCostliestEffectItem()->baseEffect->GetFormID()`와 `enchantment->GetCostliestEffectItem()->baseEffect->GetFormID()` 및 해당 MGEF의 `data.castingArt`를 나란히 출력합니다 **`[확실 / CERTAIN: RE::MagicItem::GetCostliestEffectItem, RE::Effect::baseEffect, RE::EffectSetting::data]`**.
   - **로그 판정 기준**: 두 FormID 또는 `castingArt`가 다르면(특히 `enchantment` 쪽이 `00000000`이면) 4순위가 즉시 확인됩니다.

5. **5순위(`magicNode` 3D 캐시 분리) 판별**:
   - **로그 시점 및 필드**: `state == 2`에서 `caster->GetMagicNode()` 포인터와 현재 지팡이 3D(`a_player->GetCurrent3D()` 하위 `"Weapon"` 노드의 `"MagicNode"`, `RE::NiAVObject::GetObjectByName("MagicNode")` **`[확실 / CERTAIN: RE::Actor::GetCurrent3D, RE::NiAVObject::GetObjectByName]`**) 포인터 주소를 비교 출력합니다.
   - **로그 판정 기준**: 두 포인터 주소가 불일치하거나 `GetMagicNode()->parent == nullptr` **`[확실 / CERTAIN: RE::NiAVObject::parent]`**이면 5순위가 확정됩니다.

---

### 3. 1순위 원인의 수정 방안 및 부작용 위험

#### 수정 방안 (CommonLibSSE-NG 기준)
대기 상태(`state == 0`)에서 슬롯 주문과 아트를 교체할 때, 원시 포인터만 덮어쓰지 않고 **`MagicCaster`의 주문 선택 해제 -> 슬롯·아트 갱신 -> 주문 재선택** 수명주기를 타도록 수정합니다.

1. **캐스터 유휴 상태 확인**: `staff_art.cpp:L60`과 동일하게 `caster->state.underlying() == 0` **`[확실 / CERTAIN: RE::MagicCaster::state]`**(또는 `caster->state == RE::MagicCaster::State::kNone`, 열거자명 `kNone` **`[추정 / GUESS, 미확인]`**)일 때만 아트/슬롯 교체를 수행합니다.
2. **기존 부착 아트 및 애니메이션 그래프 정상 해제**:
   - `slot`을 바꾸기 전에 `caster->DeselectSpellImpl()` **`[추정 / GUESS, 미확인: RE::MagicCaster 가상함수 vtable 0x11의 CommonLibSSE-NG 공개 메서드명 및 접근 지정자]`**을 호출하여 이전 `BGSArtObject`의 3D 노드, `SimpleAnimationGraphManagerHolder` 그래프, 진행 중인 `BGSArtObjectCloneTask`를 엔진 표준 경로로 정리합니다.
   - 만약 `magicNode` 캐시까지 강제로 비워야 한다면 `caster->ClearMagicNode()` **`[확실 / CERTAIN: RE::MagicCaster::ClearMagicNode, vtable 0x0E]`**를 함께 호출합니다.
3. **슬롯 기록 후 재선택 및 `castingArt` 최종 고정**:
   - `slot = enchantment;` 기록(이때 런타임 `enchantment`의 각 `RE::Effect::cost`도 원본과 동일하게 채워 둠 **`[확실 / CERTAIN: RE::Effect::cost]`**).
   - `caster->SelectSpellImpl()` **`[추정 / GUESS, 미확인: RE::MagicCaster vtable 0x10 메서드명]`** 호출.
   - 지팡이의 경우 `SelectSpellImpl()` 내부에서 무기 기본 인챈트(`formEnchanting` / `ExtraEnchantment`) 쪽 MGEF를 읽어 `caster->castingArt`를 채울 수 있으므로 **`[추정 / GUESS]`**, **`SelectSpellImpl()` 호출 직후에 다시 `caster->castingArt = targetArt;`를 대입**하여 의도한 아트를 확정합니다 **`[확실 / CERTAIN: RE::ActorMagicCaster::castingArt]`**. (이 순서면 공유 MGEF `00012F03`을 오염시키는 `SetRecordArt` 없이도 지팡이 캐스터의 아트를 유지할 수 있는지 바로 검증할 수 있습니다.)

#### 수정 시 깨질 수 있는 부분 (주의사항)
1. **시전 중 호출 시 애니메이션·사운드 고아 상태**: `caster->state.underlying() != 0`일 때 `DeselectSpellImpl()`이나 `ClearMagicNode()`를 호출하면 플레이어 상체 시전 애니메이션이 멈추지 않거나 `RE::MagicCaster::sounds`(`BSTArray<BSSoundHandle>`) **`[확실 / CERTAIN: RE::MagicCaster::sounds]`** 루프 사운드가 영구 잔류할 수 있습니다. 반드시 `state == 0` 가드가 필요합니다(시전 중 바인딩 변경 요청이 오면 큐에 저장했다가 `state == 0` 복귀 시 적용).
2. **무기 상시 인챈트 이펙트(`WeaponEnchantmentController`) 간섭**: 바닐라 지팡이가 아닌 모드 지팡이 중 `enchantShader` / `enchantEffectArt`를 실제로 쓰거나 `"MagicNode"` 아래에 커스텀 파티클을 붙이는 경우, `ClearMagicNode()` 호출이 `RE::ActorMagicCaster::weaponEnchantmentController`(`+0xE8`, 정확한 멤버명 **`(미확인)`** **`[추정 / GUESS]`**)의 부착 노드를 끊을 수 있으므로 `DeselectSpellImpl()`만으로 해결되면 `ClearMagicNode()`는 생략하는 편이 안전합니다.
3. **맨손 대기 이펙트 오출력 가능성**: `SelectSpellImpl()` 대신 아트 부착 함수(`CheckAttachCastingArt`, vtable `0x1B` **`[추정 / GUESS, 미확인]`**)를 `state == 0`에서 직접 강제 호출할 경우, 원래 충전 중에만 보여야 할 맨손용 대기 불꽃/냉기가 평상시 지팡이 끝에도 계속 떠 있을 수 있으므로 **`[추정 / GUESS]`**, 시전 전 대기 상태에서 지팡이 끝에 불필요한 상시 이펙트가 생기지 않는지 확인해야 합니다.
4. **양손/듀얼캐스팅 플래그 및 선택 사운드 초기화**: `DeselectSpellImpl()` -> `SelectSpellImpl()`을 거치면 `RE::MagicCaster::SetDualCasting(false)` **`[확실 / CERTAIN: RE::MagicCaster::SetDualCasting]`** 상태 리셋이나 주문 장착 효과음이 재생될 수 있습니다 **`[추정 / GUESS]`**.
