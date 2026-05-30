#pragma once
#include "../ruleengine.h"
#include <QDialog>
#include <QCheckBox>
#include <QComboBox>
#include <QList>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QTableWidget>
#include <QLineEdit>
#include <QLabel>

// ── CPU affinity topology-aware picker ────────────────────────────────────────
class AffinityDialog : public QDialog {
    Q_OBJECT
public:
    AffinityDialog(const QString &currentAffinity = {},
                   QWidget *parent = nullptr,
                   const QString &titleSuffix = {});
    QString getCpulist() const;
private:
    int              m_cpuCount = 0;
    QList<QCheckBox*>m_checkboxes;
    void selectAll();
    void selectNone();
    void selectSet(const QSet<int> &cpus);
    void validateAndAccept();
};

// ── Nice priority picker ───────────────────────────────────────────────────────
class NicePriorityDialog : public QDialog {
    Q_OBJECT
public:
    NicePriorityDialog(int currentNice = 0,
                       QWidget *parent = nullptr,
                       const QString &titleSuffix = {});
    int getNice() const { return m_spin->value(); }
private:
    QSpinBox *m_spin = nullptr;
};

// ── I/O priority picker ───────────────────────────────────────────────────────
class IoNiceDialog : public QDialog {
    Q_OBJECT
public:
    static const QList<QPair<int,QString>> CLASSES;
    IoNiceDialog(int currentClass = 2, int currentLevel = 4,
                 QWidget *parent = nullptr, const QString &titleSuffix = {});
    int getIoNiceClass() const;
    int getIoNiceLevel() const { return m_levelSpin->value(); }
private:
    QComboBox *m_classCombo = nullptr;
    QSpinBox  *m_levelSpin  = nullptr;
};

// ── Live process picker ───────────────────────────────────────────────────────
class ProcessPickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit ProcessPickerDialog(QWidget *parent = nullptr);
    QString getSelectedName()     const { return m_selectedName; }
    QString getSelectedAffinity() const { return m_selectedAffinity; }
private:
    QTableWidget *m_table = nullptr;
    QList<std::tuple<int,QString,double,QString>> m_allRows; // pid,name,cpu,affinity
    QString m_selectedName, m_selectedAffinity;
    void populate();
    void filter(const QString &text);
    void renderRows(const QList<std::tuple<int,QString,double,QString>> &rows);
    void onAccept();
};

// ── Rule add/edit dialog ──────────────────────────────────────────────────────
class RuleEditDialog : public QDialog {
    Q_OBJECT
public:
    explicit RuleEditDialog(const Rule *existingRule = nullptr,
                            QWidget *parent = nullptr);
    Rule getRule() const;
private:
    const Rule *m_rule = nullptr;
    QLineEdit  *m_nameEdit    = nullptr;
    QLineEdit  *m_patternEdit = nullptr;
    QComboBox  *m_matchCombo  = nullptr;
    QCheckBox  *m_affinityCb  = nullptr;
    QLineEdit  *m_affinityDisplay = nullptr;
    QPushButton*m_affinityPickBtn = nullptr;
    QCheckBox  *m_niceCb      = nullptr;
    QSpinBox   *m_niceSpin    = nullptr;
    QCheckBox  *m_ioniceCb    = nullptr;
    QComboBox  *m_ioniceClassCombo = nullptr;
    QSpinBox   *m_ioniceLevelSpin  = nullptr;
    QCheckBox  *m_pbExemptCb  = nullptr;
    QCheckBox  *m_enabledCb   = nullptr;
    void pickAffinity();
    void pickProcess();
    void validateAndAccept();
};

// ── Steam game picker ─────────────────────────────────────────────────────────
class SteamGamePickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit SteamGamePickerDialog(QWidget *parent = nullptr);
    QString getAppId() const { return m_appId; }
    QString getName()  const { return m_name; }
private:
    QString m_appId, m_name;
    QTableWidget *m_table = nullptr;
    QList<QPair<QString,QString>> m_allRows; // appid, name
    QLabel *m_status = nullptr;
    void scanLibrary();
    void filter(const QString &text);
    void renderRows(const QList<QPair<QString,QString>> &rows);
    void onAccept();
};

// ── Lutris game picker ────────────────────────────────────────────────────────
class LutrisGamePickerDialog : public QDialog {
    Q_OBJECT
public:
    explicit LutrisGamePickerDialog(QWidget *parent = nullptr);
    QString getSlug() const { return m_slug; }
    QString getName() const { return m_name; }
private:
    QString m_slug, m_name;
    QTableWidget *m_table = nullptr;
    QList<QPair<QString,QString>> m_allRows; // slug, displayLabel
    QLabel *m_status = nullptr;
    void scanLibrary();
    void filter(const QString &text);
    void renderRows(const QList<QPair<QString,QString>> &rows);
    void onAccept();
};

// ── Rule presets dialog ───────────────────────────────────────────────────────
struct RulePreset {
    QString name, pattern, matchType;
    QString affinity;
    std::optional<int> nice, ioniceClass, ioniceLevel;
};

class RulePresetsDialog : public QDialog {
    Q_OBJECT
public:
    explicit RulePresetsDialog(QWidget *parent = nullptr);
    const RulePreset *getPreset() const;
private:
    QTableWidget *m_table = nullptr;
    static const QList<RulePreset> PRESETS;
};
