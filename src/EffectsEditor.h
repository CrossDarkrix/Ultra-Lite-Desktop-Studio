#pragma once
#include <QDialog>
#include <QJsonObject>
#include <functional>

class EffectsWidget;
class QFormLayout;
class QTabWidget;

// Effects editor (v0.10.0): a port of Python's EffectsOverlayEditorDialog,
// scoped to the families the C++ build implements (rain / snow / cloud /
// moon / water / ice + the four scenic engines). It edits the widget's
// raw effects_json object so unknown Python-era keys survive untouched,
// applies every change live, and saves through the widget.
class EffectsEditorDialog : public QDialog {
    Q_OBJECT
public:
    explicit EffectsEditorDialog(EffectsWidget *widget, QWidget *parent = nullptr);

private:
    void commit();
    void refreshAll();
    void applyPreset(const QJsonObject &patch);

    // Bound-control builders: each registers a refresher so presets can
    // push values back into every control.
    void addCheck(QFormLayout *form, const QString &label, const QString &key, bool def);
    void addInt(QFormLayout *form, const QString &label, const QString &key, int def, int min, int max);
    void addDouble(QFormLayout *form, const QString &label, const QString &key, double def,
                   double min, double max, double step, int decimals = 2);
    void addColor(QFormLayout *form, const QString &label, const QString &key, const QString &def);
    void addSceneCheck(QFormLayout *form, const QString &label, const QString &key);

    QWidget *tabWithForm(QFormLayout **formOut, const QString &title);
    void buildThemeTab(QTabWidget *tabs);
    void buildBasicTab(QTabWidget *tabs);
    void buildRainTab(QTabWidget *tabs);
    void buildSnowTab(QTabWidget *tabs);
    void buildSkyTab(QTabWidget *tabs);
    void buildWaterIceTab(QTabWidget *tabs);
    void buildSceneTab(QTabWidget *tabs);

    EffectsWidget *widget_;
    QJsonObject obj_;
    QList<std::function<void()>> refreshers_;
    QList<class QCheckBox *> sceneBoxes_;
};
