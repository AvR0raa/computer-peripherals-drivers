"""任务 3.5 单元测试：core/profiles.py。"""
import pytest

from core.profiles import (
    DeviceProfile,
    ProfileError,
    ProfileFeature,
    list_profiles,
    load_profile,
    profile_dir,
    save_profile,
)


def make_profile(name="我的配置"):
    return DeviceProfile(
        vidpid="291D:385D",
        name=name,
        features=[
            ProfileFeature("learned_ff21_237", {"value": 1600, "mode": 2}),
            ProfileFeature("ping", {}),
        ],
    )


def test_save_and_load_roundtrip(tmp_path):
    path = save_profile(make_profile(), directory=tmp_path)
    assert path.exists()
    assert path.parent == profile_dir("291D:385D", tmp_path)
    loaded = load_profile(path)
    assert loaded.vidpid == "291D:385D"
    assert loaded.name == "我的配置"
    assert loaded.features[0].feature_id == "learned_ff21_237"
    assert loaded.features[0].params == {"value": 1600, "mode": 2}
    assert loaded.features[1].params == {}


def test_save_sanitizes_name(tmp_path):
    path = save_profile(make_profile(name='坏名字: *"?.json'), directory=tmp_path)
    assert ":" not in path.name and "*" not in path.name
    assert path.suffix == ".json"


def test_load_rejects_bad_schema(tmp_path):
    path = tmp_path / "x.json"
    path.write_text('{"$schema": "wrong"}', encoding="utf-8")
    with pytest.raises(ProfileError):
        load_profile(path)


def test_load_rejects_bad_vidpid(tmp_path):
    path = tmp_path / "x.json"
    path.write_text('{"$schema": "device-profile/v1", "vidpid": "novid"}', encoding="utf-8")
    with pytest.raises(ProfileError):
        load_profile(path)


def test_list_profiles_sorted_by_mtime(tmp_path):
    p1 = save_profile(make_profile("a"), directory=tmp_path)
    import time
    time.sleep(0.02)
    p2 = save_profile(make_profile("b"), directory=tmp_path)
    assert list_profiles("291D:385D", tmp_path) == [p2, p1]
    assert list_profiles("046D:C54D", tmp_path) == []      # 其他设备无档案


def test_empty_params_tolerated(tmp_path):
    path = save_profile(make_profile("empty"), directory=tmp_path)
    loaded = load_profile(path)
    assert loaded.features[1].params == {}
