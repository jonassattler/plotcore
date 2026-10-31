// SPDX-FileCopyrightText: 2026 Jonas Sattler
// SPDX-License-Identifier: BSD-3-Clause

#include "plotcore/qt/PlotWidget.hpp"

#include <QCursor>
#include <QEnterEvent>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <cmath>

namespace plotcore::qt {
namespace {

QColor qcolor(const Rgba& colour)
{
    return QColor::fromRgbF(static_cast<float>(colour.red), static_cast<float>(colour.green),
                            static_cast<float>(colour.blue), static_cast<float>(colour.alpha));
}

QPointF qpoint(const Point& point)
{
    return {point.x(), point.y()};
}

Modifiers modifiersOf(Qt::KeyboardModifiers keys)
{
    // The event carries the modifiers Qt last heard. A Shift held down before
    // this widget had the keyboard is not among them, so a gesture would
    // ignore it until a click gave the plot the focus. The platform is asked
    // as well, which is the keys actually down.
    keys |= QGuiApplication::queryKeyboardModifiers();
    return Modifiers{keys.testFlag(Qt::ShiftModifier), keys.testFlag(Qt::ControlModifier),
                     keys.testFlag(Qt::AltModifier)};
}

std::optional<Button> buttonOf(Qt::MouseButton button)
{
    switch (button) {
    case Qt::LeftButton:
        return Button::Left;
    case Qt::MiddleButton:
        return Button::Middle;
    case Qt::RightButton:
        return Button::Right;
    default:
        return std::nullopt;
    }
}

std::size_t slot(TextRole role)
{
    return static_cast<std::size_t>(role);
}

} // namespace

PlotWidget::PlotWidget(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::WheelFocus);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(160, 120);
    applyMeasure();
}

void PlotWidget::setRoleFont(TextRole role, const QFont& font)
{
    fonts_[slot(role)] = font;
    fontSet_[slot(role)] = true;
    applyMeasure();
    update();
}

QFont PlotWidget::roleFont(TextRole role) const
{
    if (fontSet_[slot(role)]) {
        return fonts_[slot(role)];
    }
    QFont font = this->font();
    font.setPixelSize(static_cast<int>(std::lround(figure_.style().pixelsOf(role))));
    if (role == TextRole::Title) {
        font.setWeight(QFont::DemiBold);
    }
    return font;
}

void PlotWidget::applyMeasure()
{
    // Read the fonts when asked rather than captured now, so a font changed
    // later is the one the next frame is laid out in.
    TextMeasure measure;
    measure.width = [this](const std::string& text, TextRole role) {
        return QFontMetricsF(roleFont(role)).horizontalAdvance(QString::fromStdString(text));
    };
    measure.height = [this](TextRole role) { return QFontMetricsF(roleFont(role)).height(); };
    figure_.setMeasure(std::move(measure));
}

void PlotWidget::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange) {
        update();
    }
    QWidget::changeEvent(event);
}

void PlotWidget::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // A window that opens under the pointer never gets an enter. The plot
    // takes the keyboard then, so a dock's first box does not keep it.
    if (!tookFocus_) {
        tookFocus_ = true;
        setFocus(Qt::OtherFocusReason);
    }
}

void PlotWidget::enterEvent(QEnterEvent* event)
{
    // Shift and Ctrl are this plot's gestures. They arrive only at the widget
    // that has the keyboard, so the pointer arriving is enough and a click is
    // not required first.
    setFocus(Qt::MouseFocusReason);
    QWidget::enterEvent(event);
}

void PlotWidget::followModifiers()
{
    if (QGuiApplication::mouseButtons() == Qt::NoButton) {
        return;
    }
    const QPoint at = mapFromGlobal(QCursor::pos());
    if (!rect().contains(at)) {
        return;
    }
    figure_.move(at.x(), at.y(), modifiersOf(QGuiApplication::queryKeyboardModifiers()));
    update();
}

void PlotWidget::keyPressEvent(QKeyEvent* event)
{
    const int key = event->key();
    if (!event->isAutoRepeat() &&
        (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt)) {
        followModifiers();
    }
    QWidget::keyPressEvent(event);
}

void PlotWidget::keyReleaseEvent(QKeyEvent* event)
{
    const int key = event->key();
    if (!event->isAutoRepeat() &&
        (key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt)) {
        followModifiers();
    }
    QWidget::keyReleaseEvent(event);
}

void PlotWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    update();
}

void PlotWidget::paintEvent(QPaintEvent*)
{
    figure_.setSize(width(), height(), devicePixelRatioF());
    scene_ = figure_.scene();
    QPainter painter(this);
    paintScene(painter, scene_);
}

QImage PlotWidget::render(QSize size, double scale)
{
    const double width = std::max(1, size.width());
    const double height = std::max(1, size.height());
    QImage image(static_cast<int>(std::lround(width * scale)),
                 static_cast<int>(std::lround(height * scale)),
                 QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(Qt::transparent);
    // Laid out at the size asked for, as a picture: the pane goes on being the
    // pane, and the lines are asked for nothing on the picture's account.
    SceneRequest request;
    request.picture = true;
    request.width = width;
    request.height = height;
    request.pixelRatio = scale;
    const Scene picture = figure_.scene(request);
    {
        QPainter painter(&image);
        paintScene(painter, picture);
    }
    return image;
}

void PlotWidget::drawText(QPainter& painter, const TextItem& text) const
{
    const QFont font = roleFont(text.role);
    const QFont raised = roleFont(TextRole::TickMinor);
    const QFontMetricsF metrics(font);
    const QFontMetricsF raisedMetrics(raised);
    const QString base = QString::fromStdString(text.base.empty() ? text.text : text.base);
    const QString exponent = QString::fromStdString(text.exponent);
    const double baseWidth = metrics.horizontalAdvance(base);
    const double width =
        baseWidth + (exponent.isEmpty() ? 0.0 : raisedMetrics.horizontalAdvance(exponent));
    const double height = metrics.height();

    const auto offset = [](TextItem::Align align, double extent) {
        switch (align) {
        case TextItem::Align::Middle:
            return -extent / 2.0;
        case TextItem::Align::End:
            return -extent;
        case TextItem::Align::Start:
            break;
        }
        return 0.0;
    };

    painter.save();
    painter.translate(qpoint(text.at));
    painter.rotate(text.turn);
    const double left = offset(text.across, width);
    const double top = offset(text.down, height);
    painter.setPen(qcolor(text.colour));
    painter.setFont(font);
    painter.drawText(QPointF(left, top + metrics.ascent()), base);
    if (!exponent.isEmpty()) {
        painter.setFont(raised);
        painter.drawText(QPointF(left + baseWidth, top + metrics.ascent() - text.raise), exponent);
    }
    painter.restore();
}

void PlotWidget::paintMark(QPainter& painter, const Mark& mark, const QRectF& area) const
{
    painter.save();
    if (mark.clipped) {
        painter.setClipRect(area);
    }
    if (mark.kind == Mark::Kind::Text) {
        painter.setRenderHint(QPainter::Antialiasing, true);
        drawText(painter, mark.text);
        painter.restore();
        return;
    }
    if (mark.width > 0.0) {
        QPen pen(qcolor(mark.stroke), mark.width);
        pen.setCapStyle(Qt::FlatCap);
        if (mark.dashed) {
            pen.setStyle(Qt::DashLine);
        }
        painter.setPen(pen);
    }
    else {
        painter.setPen(Qt::NoPen);
    }
    painter.setBrush(mark.fill.alpha > 0.0 ? QBrush(qcolor(mark.fill)) : QBrush(Qt::NoBrush));
    switch (mark.kind) {
    case Mark::Kind::Segment:
        // Crisp rather than smeared over two rows.
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.drawLine(qpoint(mark.from), qpoint(mark.to));
        break;
    case Mark::Kind::Box: {
        // Filled to its bounds, and edged inside them.
        const QRectF box(qpoint(mark.from), qpoint(mark.to));
        painter.setRenderHint(QPainter::Antialiasing, mark.radius > 0.0);
        const QPen edge = painter.pen();
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(box, mark.radius, mark.radius);
        if (mark.width > 0.0) {
            const double half = mark.width / 2.0;
            const double radius = std::max(0.0, mark.radius - half);
            painter.setPen(edge);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(box.adjusted(half, half, -half, -half), radius, radius);
        }
        break;
    }
    case Mark::Kind::Circle:
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.drawEllipse(qpoint(mark.from), mark.radius, mark.radius);
        break;
    case Mark::Kind::Text:
        break;
    }
    painter.restore();
}

void PlotWidget::paintScene(QPainter& painter, const Scene& scene) const
{
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (scene.ground.alpha > 0.0) {
        painter.fillRect(QRectF(0, 0, scene.width, scene.height), qcolor(scene.ground));
    }
    const QRectF area(scene.area.x, scene.area.y, scene.area.width, scene.area.height);
    for (const Mark& mark : scene.under) {
        paintMark(painter, mark, area);
    }

    // The lines, inside the pane: a run drawn past either end is clipped here
    // as the viewer's item clips it.
    painter.save();
    painter.setClipRect(area);
    painter.setRenderHint(QPainter::Antialiasing, true);
    for (const DrawnLine& line : scene.lines) {
        QPen pen(qcolor(line.colour), line.width);
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        for (const std::vector<Point>& run : line.runs) {
            if (run.size() == 1) {
                painter.drawPoint(qpoint(run.front()));
                continue;
            }
            QPolygonF polygon;
            polygon.reserve(static_cast<qsizetype>(run.size()));
            for (const Point& point : run) {
                polygon.append(qpoint(point));
            }
            painter.drawPolyline(polygon);
        }
        if (!line.markers.empty()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(qcolor(line.colour));
            for (const Point& point : line.markers) {
                painter.drawEllipse(qpoint(point), line.markerRadius, line.markerRadius);
            }
            painter.setBrush(Qt::NoBrush);
        }
    }
    painter.restore();

    for (const Mark& mark : scene.over) {
        paintMark(painter, mark, area);
    }
}

void PlotWidget::wheelEvent(QWheelEvent* event)
{
    const QPointF at = event->position();
    // Qt reports a vertical wheel with Alt held as a horizontal one.
    const QPoint delta = event->angleDelta();
    const double angle = delta.y() != 0 ? delta.y() : delta.x();
    figure_.wheel(at.x(), at.y(), angle, modifiersOf(event->modifiers()));
    event->accept();
    update();
    emit changed();
}

void PlotWidget::mousePressEvent(QMouseEvent* event)
{
    const std::optional<Button> button = buttonOf(event->button());
    if (!button.has_value()) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPointF at = event->position();
    setFocus(Qt::MouseFocusReason);
    figure_.press(at.x(), at.y(), *button, modifiersOf(event->modifiers()));
    event->accept();
    update();
}

void PlotWidget::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF at = event->position();
    figure_.move(at.x(), at.y(), modifiersOf(event->modifiers()));
    event->accept();
    update();
    if (event->buttons() != Qt::NoButton) {
        emit changed();
    }
}

void PlotWidget::mouseReleaseEvent(QMouseEvent* event)
{
    const std::optional<Button> button = buttonOf(event->button());
    if (!button.has_value()) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const QPointF at = event->position();
    figure_.release(at.x(), at.y(), *button, modifiersOf(event->modifiers()));
    event->accept();
    update();
    emit changed();
}

void PlotWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    const std::optional<Button> button = buttonOf(event->button());
    if (!button.has_value()) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    const QPointF at = event->position();
    figure_.doubleClick(at.x(), at.y(), *button);
    event->accept();
    update();
    emit changed();
}

void PlotWidget::leaveEvent(QEvent* event)
{
    figure_.leave();
    update();
    QWidget::leaveEvent(event);
}

} // namespace plotcore::qt
