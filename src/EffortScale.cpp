#include "EffortScale.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>

EffortScale::EffortScale(bool caption, QWidget *parent) : QWidget(parent) {
    auto *word = new QLabel("Effort");
    word->setObjectName("subtle");
    word->setVisible(caption);

    m_slider = new QSlider(Qt::Horizontal);
    m_slider->setObjectName("effortSlider");
    m_slider->setPageStep(1);
    m_slider->setTickPosition(QSlider::TicksBelow);
    m_slider->setTickInterval(1);
    m_slider->setFixedWidth(130);
    m_slider->setToolTip("How much the model thinks before it answers. More effort is slower (and, with Claude, "
                         "uses more of your plan); less is quicker. Default leaves it to the model.");

    m_name = new QLabel;
    m_name->setObjectName("effortName");
    m_name->setMinimumWidth(72);       // "Extra high" without the row jumping about

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(word);
    row->addWidget(m_slider);
    row->addWidget(m_name);

    connect(m_slider, &QSlider::valueChanged, this, [this] {
        showLevel();
        if (!m_slider->isSliderDown())   // a click on the groove or a key press: nothing more will follow
            commit();
    });
    connect(m_slider, &QSlider::sliderReleased, this, &EffortScale::commit);

    setBackend(LaunchConfig::Backend::LocalModel);
}

void EffortScale::setBackend(LaunchConfig::Backend backend) {
    const QString wanted = effort::clamp(m_levels.isEmpty() ? QString() : level(), backend);
    m_levels = effort::levels(backend);
    const QSignalBlocker block(m_slider);
    m_slider->setRange(0, int(m_levels.size()) - 1);
    setLevel(wanted);
}

void EffortScale::setLevel(const QString &level) {
    const QSignalBlocker block(m_slider);
    m_slider->setValue(qMax(0, int(m_levels.indexOf(level))));
    m_committed = this->level();
    showLevel();
}

QString EffortScale::level() const {
    return m_levels.value(m_slider->value());
}

void EffortScale::showLevel() {
    m_name->setText(effort::label(level()));
}

void EffortScale::commit() {
    if (level() == m_committed)
        return;
    m_committed = level();
    emit levelChanged(m_committed);
}
