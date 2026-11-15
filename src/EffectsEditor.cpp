#include "EffectsEditor.h"
#include "EffectsOverlay.h"
#include <QCheckBox>
#include <QColorDialog>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QVBoxLayout>

// Every preset starts from this base (Python: _theme_disable_all_visual_
// effects + _theme_clear_scenic_engine_flags, kept families only) merged
// over the current object, so unrelated keys are preserved.
static QJsonObject clearedBase(QJsonObject obj) {
    static const char *off[] = {
        "rain_enabled", "ripple_enabled", "rain_ripple_enabled",
        "snow_enabled", "snow_crystal_enabled",
        "cloud_enabled",
        "moon_body_enabled", "moonlight_enabled", "moon_shadow_enabled",
        "moonlight_beam_enabled",
        "water_surface_enabled", "ice_enabled", "ice_fog_enabled",
        "uyuni_salt_flat_engine_enabled", "chichibugahama_mirror_engine_enabled",
        "pamukkale_terrace_lake_engine_enabled", "sahara_desert_engine_enabled",
        "realtime_scenic_sky_enabled", "realtime_scenic_sun_rays_enabled",
    };
    for (const char *k : off) obj[QLatin1String(k)] = false;
    return obj;
}

static QJsonObject patchOf(std::initializer_list<std::pair<const char *, QJsonValue>> kv) {
    QJsonObject o;
    for (const auto &p : kv) o[QLatin1String(p.first)] = p.second;
    return o;
}

EffectsEditorDialog::EffectsEditorDialog(EffectsWidget *widget, QWidget *parent)
    : QDialog(parent), widget_(widget) {
    setWindowTitle(tr("エフェクト編集"));
    resize(600, 660);
    obj_ = widget_->effectsJson();

    auto *tabs = new QTabWidget(this);
    buildThemeTab(tabs);
    buildBasicTab(tabs);
    buildRainTab(tabs);
    buildSnowTab(tabs);
    buildSkyTab(tabs);
    buildWaterIceTab(tabs);
    buildSceneTab(tabs);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    auto *note = new QLabel(tr("変更はその場で反映され、自動で保存されます。"));
    note->setStyleSheet(QStringLiteral("color: gray;"));
    layout->addWidget(note);
    refreshAll();
}

void EffectsEditorDialog::commit() { widget_->setEffectsJson(obj_); }

void EffectsEditorDialog::refreshAll() {
    for (const auto &fn : refreshers_) fn();
}

void EffectsEditorDialog::applyPreset(const QJsonObject &patch) {
    obj_ = clearedBase(obj_);
    for (auto it = patch.begin(); it != patch.end(); ++it) obj_[it.key()] = it.value();
    commit();
    refreshAll();
}

QWidget *EffectsEditorDialog::tabWithForm(QFormLayout **formOut, const QString &title) {
    auto *page = new QWidget;
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setWidget(page);
    auto *form = new QFormLayout(page);
    form->setLabelAlignment(Qt::AlignRight);
    *formOut = form;
    auto *tabs = qobject_cast<QTabWidget *>(parentWidget());
    Q_UNUSED(tabs);
    // The caller adds `scroll` to the tab widget via buildXTab helpers.
    page->setProperty("scroll", QVariant::fromValue(static_cast<void *>(scroll)));
    return page;
}

// --- bound controls -------------------------------------------------

void EffectsEditorDialog::addCheck(QFormLayout *form, const QString &label, const QString &key, bool def) {
    auto *box = new QCheckBox(label);
    form->addRow(box);
    auto read = [this, key, def] { return obj_.value(key).toBool(def); };
    refreshers_.append([box, read] {
        QSignalBlocker b(box);
        box->setChecked(read());
    });
    connect(box, &QCheckBox::toggled, this, [this, key](bool on) {
        obj_[key] = on;
        commit();
    });
    box->setChecked(read());
}

void EffectsEditorDialog::addInt(QFormLayout *form, const QString &label, const QString &key,
                                 int def, int min, int max) {
    auto *spin = new QSpinBox;
    spin->setRange(min, max);
    form->addRow(label, spin);
    auto read = [this, key, def] { return obj_.value(key).toInt(def); };
    refreshers_.append([spin, read] {
        QSignalBlocker b(spin);
        spin->setValue(read());
    });
    connect(spin, &QSpinBox::valueChanged, this, [this, key](int v) {
        obj_[key] = v;
        commit();
    });
    spin->setValue(read());
}

void EffectsEditorDialog::addDouble(QFormLayout *form, const QString &label, const QString &key,
                                    double def, double min, double max, double step, int decimals) {
    auto *spin = new QDoubleSpinBox;
    spin->setRange(min, max);
    spin->setSingleStep(step);
    spin->setDecimals(decimals);
    form->addRow(label, spin);
    auto read = [this, key, def] { return obj_.value(key).toDouble(def); };
    refreshers_.append([spin, read] {
        QSignalBlocker b(spin);
        spin->setValue(read());
    });
    connect(spin, &QDoubleSpinBox::valueChanged, this, [this, key](double v) {
        obj_[key] = v;
        commit();
    });
    spin->setValue(read());
}

void EffectsEditorDialog::addColor(QFormLayout *form, const QString &label, const QString &key,
                                   const QString &def) {
    auto *btn = new QPushButton;
    form->addRow(label, btn);
    auto read = [this, key, def] { return obj_.value(key).toString(def); };
    auto paint = [btn, read] {
        btn->setText(read());
        btn->setStyleSheet(QStringLiteral("background: %1;").arg(read()));
    };
    refreshers_.append(paint);
    connect(btn, &QPushButton::clicked, this, [this, key, read, paint] {
        const QColor c = QColorDialog::getColor(QColor(read()), this, key);
        if (c.isValid()) {
            obj_[key] = c.name().toUpper();
            paint();
            commit();
        }
    });
    paint();
}

void EffectsEditorDialog::addSceneCheck(QFormLayout *form, const QString &label, const QString &key) {
    auto *box = new QCheckBox(label);
    form->addRow(box);
    sceneBoxes_.append(box);
    auto read = [this, key] { return obj_.value(key).toBool(false); };
    refreshers_.append([box, read] {
        QSignalBlocker b(box);
        box->setChecked(read());
    });
    connect(box, &QCheckBox::toggled, this, [this, box, key](bool on) {
        // Scenic engines are mutually exclusive (Python's presets
        // select exactly one); checking one clears the others.
        static const char *keys[] = {
            "uyuni_salt_flat_engine_enabled", "chichibugahama_mirror_engine_enabled",
            "pamukkale_terrace_lake_engine_enabled", "sahara_desert_engine_enabled",
        };
        for (const char *k : keys) obj_[QLatin1String(k)] = false;
        obj_[key] = on;
        commit();
        refreshAll();
        Q_UNUSED(box);
    });
    box->setChecked(read());
}

// --- tabs -----------------------------------------------------------

void EffectsEditorDialog::buildThemeTab(QTabWidget *tabs) {
    auto *page = new QWidget;
    auto *grid = new QGridLayout(page);
    auto *head = new QLabel(tr("テーマを選ぶと、複数の効果がまとめて設定されます (Python版のプリセットから、実装済みの系統だけで再構成したものです)。"));
    head->setWordWrap(true);
    grid->addWidget(head, 0, 0, 1, 2);
    struct Preset { QString label; QJsonObject patch; };
    const QList<Preset> presets = {
        {QStringLiteral("🌙 静かな夜空"), patchOf({
            {"effect_frame_rate", 30},
            {"moon_body_enabled", true}, {"moonlight_enabled", true}, {"moon_shadow_enabled", true},
            {"moon_alpha", 225}, {"moonlight_alpha", 70}})},
        {QStringLiteral("🌌 月夜の水面"), patchOf({
            {"effect_frame_rate", 30},
            {"moon_body_enabled", true}, {"moonlight_enabled", true}, {"moon_shadow_enabled", true},
            {"water_surface_enabled", true}, {"water_surface_alpha", 104},
            {"water_surface_wave_count", 12}, {"water_surface_wave_height", 10.0}})},
        {QStringLiteral("🌧 雨と波紋"), patchOf({
            {"effect_frame_rate", 30},
            {"rain_enabled", true}, {"rain_count", 85}, {"rain_speed", 1.12},
            {"ripple_enabled", true}, {"rain_ripple_enabled", true}, {"rain_ripple_chance", 0.70},
            {"water_surface_enabled", true}, {"water_surface_alpha", 96}, {"water_surface_wave_count", 14}})},
        {QStringLiteral("❄ 雪景色"), patchOf({
            {"effect_frame_rate", 30},
            {"snow_enabled", true}, {"snow_count", 120}, {"snow_speed", 0.16},
            {"snow_size", 4.2}, {"snow_alpha", 210},
            {"snow_crystal_enabled", true}, {"snow_crystal_count", 28}, {"snow_crystal_speed", 0.11},
            {"snow_crystal_size", 13.0}, {"snow_crystal_alpha", 220},
            {"ripple_enabled", true},
            {"moon_body_enabled", true}, {"moonlight_enabled", true},
            {"water_surface_enabled", true}, {"water_surface_alpha", 62}})},
        {QStringLiteral("🧊 氷河の鏡面"), patchOf({
            {"effect_frame_rate", 30},
            {"ice_enabled", true}, {"ice_alpha", 185}, {"ice_y", 0.56},
            {"ice_crack_intensity", 0.58}, {"ice_bubble_intensity", 0.42},
            {"ice_fog_enabled", true}, {"ice_fog_alpha", 76},
            {"snow_enabled", true}, {"snow_count", 70}, {"snow_speed", 0.13},
            {"snow_size", 3.8}, {"snow_alpha", 185},
            {"snow_crystal_enabled", true}, {"snow_crystal_count", 14}, {"snow_crystal_speed", 0.09},
            {"snow_crystal_size", 12.0}, {"snow_crystal_alpha", 200}})},
        {QStringLiteral("☁ 雲の流れ"), patchOf({
            {"effect_frame_rate", 30},
            {"cloud_enabled", true}, {"cloud_count", 12}, {"cloud_speed", 0.865},
            {"cloud_size", 105.0}, {"cloud_alpha", 190},
            {"cloud_color", QStringLiteral("#F4FAFF")}, {"cloud_shadow_color", QStringLiteral("#AEBED2")},
            {"cloud_highlight_color", QStringLiteral("#FFFFFF")},
            {"cloud_altitude", 0.24}, {"cloud_depth", 0.48}})},
        {QStringLiteral("🪞 ウユニ塩湖の静かな反射"), patchOf({
            {"effect_frame_rate", 45},
            {"uyuni_salt_flat_engine_enabled", true}, {"realtime_scenic_sky_enabled", true},
            {"water_surface_enabled", true}, {"water_surface_alpha", 52},
            {"water_surface_color", QStringLiteral("#EAFBFF")},
            {"water_surface_highlight_color", QStringLiteral("#FFFFFF")},
            {"water_surface_flow_speed", 0.035}, {"water_surface_wave_count", 4},
            {"water_surface_wave_height", 1.8}, {"water_surface_y", 0.50}, {"water_surface_depth", 0.50},
            {"ice_enabled", true}, {"ice_alpha", 36},
            {"ice_color", QStringLiteral("#F7FFFF")}, {"ice_edge_color", QStringLiteral("#FFFFFF")},
            {"ice_highlight_color", QStringLiteral("#FFFFFF")},
            {"ice_y", 0.50}, {"ice_crack_intensity", 0.08}, {"ice_bubble_intensity", 0.0},
            {"cloud_enabled", true}, {"cloud_count", 6}, {"cloud_speed", 0.865},
            {"cloud_size", 112.0}, {"cloud_alpha", 176}})},
        {QStringLiteral("🏜 サハラ砂漠の灼熱日光"), patchOf({
            {"effect_frame_rate", 40},
            {"sahara_desert_engine_enabled", true}, {"realtime_scenic_sky_enabled", true},
            {"cloud_enabled", true}, {"cloud_count", 5}})},
        {QStringLiteral("🏞 パムッカレの棚田な湖"), patchOf({
            {"effect_frame_rate", 60},
            {"pamukkale_terrace_lake_engine_enabled", true}})},
        {QStringLiteral("🪞 父母ヶ浜の鏡面反射する水辺"), patchOf({
            {"effect_frame_rate", 45},
            {"chichibugahama_mirror_engine_enabled", true}, {"realtime_scenic_sky_enabled", true},
            {"scenic_atmosphere_enhancement_enabled", true},
            {"cloud_enabled", true}, {"cloud_count", 5}, {"cloud_speed", 0.865},
            {"cloud_size", 104.0}, {"cloud_alpha", 158}})},
        {QStringLiteral("⏹ すべてオフ"), QJsonObject()},
    };
    for (int i = 0; i < presets.size(); ++i) {
        auto *btn = new QPushButton(presets[i].label);
        grid->addWidget(btn, 1 + i / 2, i % 2);
        const QJsonObject patch = presets[i].patch;
        connect(btn, &QPushButton::clicked, this, [this, patch] { applyPreset(patch); });
    }
    grid->setRowStretch(grid->rowCount(), 1);
    tabs->addTab(page, tr("テーマ"));
}

void EffectsEditorDialog::buildBasicTab(QTabWidget *tabs) {
    QFormLayout *form = nullptr;
    QWidget *page = tabWithForm(&form, tr("基本"));
    addCheck(form, tr("フレームレート制限を使う"), QStringLiteral("effect_frame_rate_enabled"), true);
    addInt(form, tr("フレームレート (fps)"), QStringLiteral("effect_frame_rate"), 24, 5, 60);
    addCheck(form, tr("デスクトップアイコンをシーンに描く"), QStringLiteral("icon_scene_enabled"), true);
    addCheck(form, tr("シーンの大気 (霧) を強化"), QStringLiteral("scenic_atmosphere_enhancement_enabled"), true);
    addCheck(form, tr("リアルタイムの空を使う"), QStringLiteral("realtime_scenic_sky_enabled"), false);
    addCheck(form, tr("太陽光線を描く"), QStringLiteral("realtime_scenic_sun_rays_enabled"), false);
    addDouble(form, tr("水鏡の波の強さ"), QStringLiteral("water_mirror_wave"), 7.0, 0.0, 30.0, 0.5);
    tabs->addTab(static_cast<QScrollArea *>(page->property("scroll").value<void *>()), tr("基本"));
}

void EffectsEditorDialog::buildRainTab(QTabWidget *tabs) {
    QFormLayout *form = nullptr;
    QWidget *page = tabWithForm(&form, tr("雨"));
    addCheck(form, tr("雨を有効化"), QStringLiteral("rain_enabled"), false);
    addInt(form, tr("雨の数"), QStringLiteral("rain_count"), 90, 0, 600);
    addDouble(form, tr("雨の速さ"), QStringLiteral("rain_speed"), 1.0, 0.1, 3.0, 0.05);
    addDouble(form, tr("雨の長さ"), QStringLiteral("rain_length"), 16.0, 2.0, 60.0, 1.0);
    addDouble(form, tr("粒の太さ (最小)"), QStringLiteral("rain_drop_min_size"), 1.0, 0.2, 10.0, 0.1);
    addDouble(form, tr("粒の太さ (最大)"), QStringLiteral("rain_drop_max_size"), 2.4, 0.2, 10.0, 0.1);
    addDouble(form, tr("長さのばらつき"), QStringLiteral("rain_drop_length_randomness"), 0.55, 0.0, 1.0, 0.05);
    addColor(form, tr("雨の色"), QStringLiteral("rain_color"), QStringLiteral("#9FD7FF"));
    addCheck(form, tr("波紋を有効化 (全体)"), QStringLiteral("ripple_enabled"), false);
    addCheck(form, tr("雨の波紋を有効化"), QStringLiteral("rain_ripple_enabled"), false);
    addDouble(form, tr("波紋の発生率"), QStringLiteral("rain_ripple_chance"), 0.55, 0.0, 1.0, 0.05);
    addDouble(form, tr("波紋の水面の高さ"), QStringLiteral("rain_ripple_surface_y"), 0.82, 0.0, 1.0, 0.01);
    addDouble(form, tr("波紋の半径 (最小)"), QStringLiteral("rain_ripple_min_radius"), 24.0, 1.0, 300.0, 1.0);
    addDouble(form, tr("波紋の半径 (最大)"), QStringLiteral("rain_ripple_max_radius"), 92.0, 1.0, 400.0, 1.0);
    addDouble(form, tr("波紋の速さ"), QStringLiteral("ripple_speed"), 1.0, 0.1, 3.0, 0.05);
    addColor(form, tr("波紋の色"), QStringLiteral("ripple_color"), QStringLiteral("#A8EFFF"));
    tabs->addTab(static_cast<QScrollArea *>(page->property("scroll").value<void *>()), tr("雨と波紋"));
}

void EffectsEditorDialog::buildSnowTab(QTabWidget *tabs) {
    QFormLayout *form = nullptr;
    QWidget *page = tabWithForm(&form, tr("雪"));
    addCheck(form, tr("雪を有効化"), QStringLiteral("snow_enabled"), false);
    addInt(form, tr("雪の数"), QStringLiteral("snow_count"), 90, 0, 600);
    addDouble(form, tr("雪の速さ"), QStringLiteral("snow_speed"), 0.18, 0.01, 2.0, 0.01);
    addDouble(form, tr("雪の大きさ"), QStringLiteral("snow_size"), 4.5, 0.5, 20.0, 0.1);
    addInt(form, tr("雪の不透明度"), QStringLiteral("snow_alpha"), 210, 0, 255);
    addColor(form, tr("雪の色"), QStringLiteral("snow_color"), QStringLiteral("#F5FCFF"));
    addColor(form, tr("雪の縁の色"), QStringLiteral("snow_edge_color"), QStringLiteral("#CFEFFF"));
    addDouble(form, tr("積もる高さ"), QStringLiteral("snow_surface_y"), 0.86, 0.0, 1.0, 0.01);
    addCheck(form, tr("雪の結晶を有効化"), QStringLiteral("snow_crystal_enabled"), false);
    addInt(form, tr("結晶の数"), QStringLiteral("snow_crystal_count"), 22, 0, 300);
    addDouble(form, tr("結晶の速さ"), QStringLiteral("snow_crystal_speed"), 0.12, 0.01, 2.0, 0.01);
    addDouble(form, tr("結晶の大きさ"), QStringLiteral("snow_crystal_size"), 15.0, 2.0, 60.0, 0.5);
    addInt(form, tr("結晶の不透明度"), QStringLiteral("snow_crystal_alpha"), 220, 0, 255);
    addColor(form, tr("結晶の色"), QStringLiteral("snow_crystal_color"), QStringLiteral("#EBFAFF"));
    addColor(form, tr("結晶の縁の色"), QStringLiteral("snow_crystal_edge_color"), QStringLiteral("#D8F4FF"));
    tabs->addTab(static_cast<QScrollArea *>(page->property("scroll").value<void *>()), tr("雪"));
}

void EffectsEditorDialog::buildSkyTab(QTabWidget *tabs) {
    QFormLayout *form = nullptr;
    QWidget *page = tabWithForm(&form, tr("雲と月"));
    addCheck(form, tr("雲を有効化"), QStringLiteral("cloud_enabled"), false);
    addInt(form, tr("雲の数"), QStringLiteral("cloud_count"), 13, 0, 100);
    addDouble(form, tr("雲の速さ"), QStringLiteral("cloud_speed"), 1.05, 0.0, 3.0, 0.05);
    addDouble(form, tr("雲の大きさ"), QStringLiteral("cloud_size"), 120.0, 20.0, 400.0, 2.0);
    addInt(form, tr("雲の不透明度"), QStringLiteral("cloud_alpha"), 200, 0, 255);
    addColor(form, tr("雲の色"), QStringLiteral("cloud_color"), QStringLiteral("#F4FAFF"));
    addColor(form, tr("雲の影の色"), QStringLiteral("cloud_shadow_color"), QStringLiteral("#B8C7D8"));
    addColor(form, tr("雲の光の色"), QStringLiteral("cloud_highlight_color"), QStringLiteral("#FFFFFF"));
    addDouble(form, tr("雲の高さ"), QStringLiteral("cloud_altitude"), 0.22, 0.0, 1.0, 0.01);
    addDouble(form, tr("雲の帯の幅"), QStringLiteral("cloud_depth"), 0.42, 0.0, 1.0, 0.01);
    addCheck(form, tr("月を有効化"), QStringLiteral("moon_body_enabled"), false);
    addCheck(form, tr("月光を有効化"), QStringLiteral("moonlight_enabled"), false);
    addCheck(form, tr("月の影を有効化"), QStringLiteral("moon_shadow_enabled"), false);
    addDouble(form, tr("月の位置 X"), QStringLiteral("moon_x"), 0.78, 0.0, 1.0, 0.01);
    addDouble(form, tr("月の位置 Y"), QStringLiteral("moon_y"), 0.18, 0.0, 1.0, 0.01);
    addDouble(form, tr("月の半径"), QStringLiteral("moon_radius"), 74.0, 5.0, 300.0, 1.0);
    addInt(form, tr("月の不透明度"), QStringLiteral("moon_alpha"), 230, 0, 255);
    addColor(form, tr("月の色"), QStringLiteral("moon_color"), QStringLiteral("#FFF3C4"));
    addColor(form, tr("月の縁の色"), QStringLiteral("moon_edge_color"), QStringLiteral("#C9D7FF"));
    addInt(form, tr("クレーターの数"), QStringLiteral("moon_crater_count"), 9, 0, 40);
    addInt(form, tr("クレーターの濃さ"), QStringLiteral("moon_crater_alpha"), 54, 0, 255);
    addDouble(form, tr("月光の半径"), QStringLiteral("moonlight_radius"), 260.0, 20.0, 900.0, 5.0);
    addInt(form, tr("月光の強さ"), QStringLiteral("moonlight_alpha"), 82, 0, 255);
    addColor(form, tr("月光の色"), QStringLiteral("moonlight_color"), QStringLiteral("#CFE8FF"));
    addCheck(form, tr("月の光条を有効化"), QStringLiteral("moonlight_beam_enabled"), false);
    addInt(form, tr("光条の強さ"), QStringLiteral("moonlight_beam_alpha"), 44, 0, 255);
    addDouble(form, tr("光条の幅"), QStringLiteral("moonlight_beam_width"), 0.34, 0.0, 1.0, 0.02);
    addColor(form, tr("月の影の色"), QStringLiteral("moon_shadow_color"), QStringLiteral("#061028"));
    addInt(form, tr("月の影の濃さ"), QStringLiteral("moon_shadow_alpha"), 70, 0, 255);
    tabs->addTab(static_cast<QScrollArea *>(page->property("scroll").value<void *>()), tr("雲と月"));
}

void EffectsEditorDialog::buildWaterIceTab(QTabWidget *tabs) {
    QFormLayout *form = nullptr;
    QWidget *page = tabWithForm(&form, tr("水と氷"));
    addCheck(form, tr("水面を有効化"), QStringLiteral("water_surface_enabled"), false);
    addInt(form, tr("水面の不透明度"), QStringLiteral("water_surface_alpha"), 92, 0, 255);
    addColor(form, tr("水面の色"), QStringLiteral("water_surface_color"), QStringLiteral("#4FC3FF"));
    addColor(form, tr("水面の光の色"), QStringLiteral("water_surface_highlight_color"), QStringLiteral("#D8FAFF"));
    addDouble(form, tr("水面の高さ"), QStringLiteral("water_surface_y"), 0.58, 0.0, 1.0, 0.01);
    addDouble(form, tr("水面の深さ"), QStringLiteral("water_surface_depth"), 0.42, 0.0, 1.0, 0.01);
    addDouble(form, tr("流れの速さ"), QStringLiteral("water_surface_flow_speed"), 0.55, 0.0, 3.0, 0.05);
    addDouble(form, tr("流れの角度"), QStringLiteral("water_surface_flow_angle"), 0.0, -180.0, 180.0, 1.0);
    addInt(form, tr("波の数"), QStringLiteral("water_surface_wave_count"), 14, 0, 60);
    addDouble(form, tr("波の高さ"), QStringLiteral("water_surface_wave_height"), 12.0, 0.0, 60.0, 0.5);
    addCheck(form, tr("氷面を有効化"), QStringLiteral("ice_enabled"), false);
    addInt(form, tr("氷の不透明度"), QStringLiteral("ice_alpha"), 178, 0, 255);
    addColor(form, tr("氷の色"), QStringLiteral("ice_color"), QStringLiteral("#9BDDF2"));
    addColor(form, tr("氷の縁の色"), QStringLiteral("ice_edge_color"), QStringLiteral("#E8FBFF"));
    addColor(form, tr("氷の光の色"), QStringLiteral("ice_highlight_color"), QStringLiteral("#F7FFFF"));
    addDouble(form, tr("氷面の高さ"), QStringLiteral("ice_y"), 0.58, 0.0, 1.0, 0.01);
    addDouble(form, tr("ひびの強さ"), QStringLiteral("ice_crack_intensity"), 0.46, 0.0, 1.0, 0.02);
    addDouble(form, tr("気泡の強さ"), QStringLiteral("ice_bubble_intensity"), 0.36, 0.0, 1.0, 0.02);
    addCheck(form, tr("氷霧を有効化"), QStringLiteral("ice_fog_enabled"), false);
    addInt(form, tr("氷霧の濃さ"), QStringLiteral("ice_fog_alpha"), 72, 0, 255);
    addDouble(form, tr("氷霧の高さ"), QStringLiteral("ice_fog_height"), 0.24, 0.0, 1.0, 0.01);
    addColor(form, tr("氷霧の色"), QStringLiteral("ice_fog_color"), QStringLiteral("#EEF9FF"));
    tabs->addTab(static_cast<QScrollArea *>(page->property("scroll").value<void *>()), tr("水と氷"));
}

void EffectsEditorDialog::buildSceneTab(QTabWidget *tabs) {
    QFormLayout *form = nullptr;
    QWidget *page = tabWithForm(&form, tr("シーン"));
    auto *note = new QLabel(tr("全画面シーンは1つだけ選べます。空と大気の設定は「基本」タブにあります。"));
    note->setWordWrap(true);
    form->addRow(note);
    addSceneCheck(form, tr("ウユニ塩湖"), QStringLiteral("uyuni_salt_flat_engine_enabled"));
    addSceneCheck(form, tr("父母ヶ浜"), QStringLiteral("chichibugahama_mirror_engine_enabled"));
    addSceneCheck(form, tr("パムッカレ"), QStringLiteral("pamukkale_terrace_lake_engine_enabled"));
    addSceneCheck(form, tr("サハラ砂漠"), QStringLiteral("sahara_desert_engine_enabled"));
    tabs->addTab(static_cast<QScrollArea *>(page->property("scroll").value<void *>()), tr("シーン"));
}
