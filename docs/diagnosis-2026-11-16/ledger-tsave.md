# T-SAVE 원장 (SCSSoulCharge standalone, 2026-11-16)

## 확인됨
- 복사본 FF0039E6에서 나온 효과 7개는 같은 실행 안 재로드에서 유지, 재시작 뒤 0개(578→571, 592→585). 근거: SCSSoulCharge.log 5065, 5499, 5541, 5854, 6289, 6741.
- 재시작 뒤 손 칸 포인터는 null이 아니고 FormID FF0039E6, 이름 ''(GetName 호출에 튕김 없음). 근거: 5853.
- CommonLib `MagicTarget::AddTargetData`에 `magicItem`(08), `effect`(10) 있음, `AddTarget`은 vfunc 01. 근거: extern/CommonLibSSE-NG/include/RE/M/MagicTarget.h:90-115. 공개 모드가 `VTABLE_Character[4]`/`VTABLE_PlayerCharacter[4]` 0x1로 훅함(TACTICDataCollection ParalysisBlockHook.cpp:90-97).
- SKSE `kSaveGame`은 엔진 저장 호출 전에 보냄. 근거: ianpatt/skse64 Hooks_SaveLoad.cpp:25-26. `kPostLoadGame`은 로드 함수가 끝난 뒤(:50-51).
- `BGSCreatedObjectManager`에는 `AddWeaponEnchantment`/`AddArmorEnchantment`/`DestroyEnchantment`만 있고 마법부여 참조 증감 함수는 없음(IncrementRef는 AlchemyItem용). 근거: BGSCreatedObjectManager.h:21-63.

## 가설
- H1 (Carbon, GPT 공통): 복사본의 효과 목록은 세이브에 저장되지 않는다. 같은 실행에서는 메모리에 남은 복사본으로 이어 붙고, 재시작 뒤에는 효과 정의가 없어 7개가 버려진다. 확인: 고친 시험판에서 재시작 뒤 효과 유지 여부(T-SAVE-COLD).
- H2 (Carbon, 추정): 재시작 뒤 손 칸의 FF0039E6은 `AddChange(1)` 때문에 세이브에서 되살아난 빈 껍데기 마법부여다. 대안(GPT): 같은 번호를 받은 다른 폼, 또는 죽은 포인터. 확인: 로드 뒤 칸의 폼 종류, effects 개수, `LookupByID(id)==slot` 로그. 비용: 시험판 로그 한 줄.
- H3 (Carbon): co-save 로드는 효과를 읽기 전에 돈다(로그 2.8초 차이만 근거). 1순위 해법에는 필요 없음. 미확인.
- ~~GPT 함수 이름 `CreateWeaponEnchantment`, `IncrementEnchantmentReference`~~: 헤더에 없음(실제는 AddWeaponEnchantment, 참조 함수 없음).

## 해법 후보
1. AddTarget 훅에서 효과를 원본 주문과 그 effects[i]로 다시 연결 + AddChange(1) 삭제 + 저장 직전 손 칸을 지팡이 원래 마법부여로 되돌리고 다음 프레임에 복구. (Carbon 1순위, GPT 3순위. 이름 확인 끝)
2. 제작 마법부여(BGSCreatedObjectManager). (GPT 1순위, Carbon 최하위: 무기·방어구 형식만, 지팡이 형식 보존 미확인)
