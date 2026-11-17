#pragma once
#include <QStringList>
#include <QWidget>
#include "LaunchConfig.h"

class QLabel;
class QSlider;

// The effort scale: a slider with one stop per level the backend has (see
// `effort` in LaunchConfig.h), and the name of the chosen level next to it.
// The first stop is "Default": leave it to the model.
class EffortScale : public QWidget {
    Q_OBJECT
public:
    // `caption`: show the word "Effort" in front (not needed where a form already labels the row).
    explicit EffortScale(bool caption = true, QWidget *parent = nullptr);

    // Which backend's levels the scale shows. The level moves to the nearest
    // one the new backend has.
    void setBackend(LaunchConfig::Backend backend);
    // Neither of these emits levelChanged().
    void setLevel(const QString &level);
    QString level() const;

signals:
    // The user chose another level (while dragging, only once the handle is let go).
    void levelChanged(const QString &level);

private:
    void showLevel();
    void commit();

    QSlider    *m_slider = nullptr;
    QLabel     *m_name   = nullptr;
    QStringList m_levels;
    QString     m_committed;       // the level last reported or set
};
