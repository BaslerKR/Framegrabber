#pragma once

#include <QAbstractSpinBox>
#include <QLineEdit>
#include <QSignalBlocker>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>

namespace framegrabber::presentation {

/** SDK-independent numeric editor interface used by runtime and presentation consumers. */
class NumericFeatureEditor : public QAbstractSpinBox
{
public:
    using QAbstractSpinBox::QAbstractSpinBox;
    /** @brief Commits valid input, or restores the prior value without requesting a write. */
    virtual std::optional<QString> commitText() = 0;
    /** @brief Restores an accepted SDK value after a rejected asynchronous write. */
    virtual void restoreText(const QString& text) = 0;
};

/** Preserves native integer width and floating-point round trips without decimal rounding. */
template<typename T>
class FeatureSpinBox final : public NumericFeatureEditor
{
public:
    FeatureSpinBox(T value, std::optional<T> minimum = {}, std::optional<T> maximum = {},
                   std::optional<T> increment = {}, QWidget* parent = nullptr)
        : NumericFeatureEditor(parent), value_(value), minimum_(minimum), maximum_(maximum), increment_(increment)
    {
        if (minimum_ && !finite(*minimum_)) minimum_.reset();
        if (maximum_ && !finite(*maximum_)) maximum_.reset();
        if (minimum_ && maximum_ && *minimum_ > *maximum_) {
            minimum_.reset(); maximum_.reset();
        }
        if (increment_ && (!finite(*increment_) || *increment_ <= 0)) increment_.reset();
        setKeyboardTracking(false);
        setFocusPolicy(Qt::StrongFocus);
        setAlignment(Qt::AlignRight);
        setValue(value);
    }

    /** @brief Returns the typed displayed value without narrowing or decimal rounding. */
    T value() const { return value_; }
    void restoreText(const QString& input) override {
        if (const auto parsed = parse(input)) setValue(*parsed);
    }
    /** @brief Displays a value without emitting an editing or write notification. */
    void setValue(T value) {
        value_ = value;
        const QSignalBlocker blocker(lineEdit());
        lineEdit()->setText(text(value));
        // Narrow feature columns may scroll long values; hover still exposes every digit.
        setToolTip(QStringLiteral("Value: %1\nMinimum: %2\nMaximum: %3\nIncrement: %4")
            .arg(text(value_), minimum_ ? text(*minimum_) : QStringLiteral("Not reported"),
                 maximum_ ? text(*maximum_) : QStringLiteral("Not reported"),
                 increment_ ? text(*increment_) : QStringLiteral("Not reported")));
    }

    std::optional<QString> commitText() override {
        const auto parsed = parse(lineEdit()->text());
        if (!parsed || !acceptable(*parsed)) { setValue(value_); return {}; }
        setValue(*parsed);
        return text(value_);
    }

    void stepBy(int steps) override {
        if (!isEnabled() || isReadOnly() || steps == 0) return;
        if (!commitText()) return;
        const T low = minimum_.value_or((std::numeric_limits<T>::lowest)());
        const T high = maximum_.value_or((std::numeric_limits<T>::max)());
        if (value_ < low || value_ > high) return;
        T next = value_;
        if constexpr (std::is_integral_v<T>) {
            const auto position = ordinal(value_);
            const auto room = steps > 0 ? ordinal(high) - position : position - ordinal(low);
            const auto count = steps > 0 ? std::uint64_t(steps) : std::uint64_t(-std::int64_t(steps));
            const auto increment = std::uint64_t(increment_.value_or(1));
            // Stop at the last complete step; never overflow or land off the SDK grid.
            const auto distance = (std::min)(count, room / increment) * increment;
            next = fromOrdinal(steps > 0 ? position + distance : position - distance);
        } else {
            const long double step = increment_.value_or(1.0);
            const long double room = steps > 0 ? static_cast<long double>(high) - value_
                                               : static_cast<long double>(value_) - low;
            const long double available = room / step;
            const long double tolerance = std::numeric_limits<T>::epsilon()
                * (std::max)(1.0L, std::abs(available)) * 8;
            const long double count = (std::min)(std::abs(static_cast<long double>(steps)),
                std::floor(available + tolerance));
            const long double candidate = static_cast<long double>(value_) + (steps > 0 ? count : -count) * step;
            next = static_cast<T>((std::clamp)(candidate, static_cast<long double>(low), static_cast<long double>(high)));
        }
        if (next == value_ || !acceptable(next)) return;
        setValue(next);
        emit editingFinished();
    }

    StepEnabled stepEnabled() const override {
        if (!isEnabled() || isReadOnly()) return StepNone;
        StepEnabled result = StepNone;
        const T low = minimum_.value_or((std::numeric_limits<T>::lowest)());
        const T high = maximum_.value_or((std::numeric_limits<T>::max)());
        if (value_ < low || value_ > high) return result;
        if constexpr (std::is_integral_v<T>) {
            const auto step = std::uint64_t(increment_.value_or(1));
            if (ordinal(high) - ordinal(value_) >= step) result |= StepUpEnabled;
            if (ordinal(value_) - ordinal(low) >= step) result |= StepDownEnabled;
        } else {
            const long double step = increment_.value_or(1.0);
            if (static_cast<long double>(value_) + step <= high) result |= StepUpEnabled;
            if (static_cast<long double>(value_) - step >= low) result |= StepDownEnabled;
        }
        return result;
    }

    QValidator::State validate(QString& input, int&) const override {
        if (input.isEmpty() || input == "-" || input == "+") return QValidator::Intermediate;
        const auto parsed = parse(input);
        // Allow incomplete scientific notation and intermediate edits; commit rejects invalid values.
        return parsed && acceptable(*parsed) ? QValidator::Acceptable : QValidator::Intermediate;
    }
    void fixup(QString& input) const override { input = text(value_); }

protected:
    void wheelEvent(QWheelEvent* event) override {
        if (!hasFocus()) { event->ignore(); return; }
        QAbstractSpinBox::wheelEvent(event);
    }

private:
    static bool finite(T value) {
        if constexpr (std::is_floating_point_v<T>) return std::isfinite(value);
        return true;
    }
    static QString text(T value) {
        if constexpr (std::is_floating_point_v<T>) return QString::number(value, 'g', 17);
        else if constexpr (std::is_signed_v<T>) return QString::number(qlonglong(value));
        else return QString::number(qulonglong(value));
    }
    static std::optional<T> parse(const QString& input) {
        bool ok = false;
        T value;
        if constexpr (std::is_floating_point_v<T>) value = input.toDouble(&ok);
        else if constexpr (std::is_signed_v<T>) {
            const auto parsed = input.toLongLong(&ok);
            if (parsed < (std::numeric_limits<T>::lowest)() || parsed > (std::numeric_limits<T>::max)()) return {};
            value = static_cast<T>(parsed);
        }
        else {
            if (input.trimmed().startsWith('-')) return {};
            const auto parsed = input.toULongLong(&ok);
            if (parsed > (std::numeric_limits<T>::max)()) return {};
            value = static_cast<T>(parsed);
        }
        return ok && finite(value) ? std::optional<T>(value) : std::nullopt;
    }
    bool acceptable(T value) const {
        if (!finite(value) || (minimum_ && value < *minimum_) || (maximum_ && value > *maximum_)) return false;
        if (minimum_ && increment_) {
            if constexpr (std::is_integral_v<T>) {
                return (ordinal(value) - ordinal(*minimum_)) % std::uint64_t(*increment_) == 0;
            } else {
                const long double count = (static_cast<long double>(value) - *minimum_) / *increment_;
                const long double tolerance = std::numeric_limits<double>::epsilon()
                    * (std::max)(1.0L, std::abs(count)) * 8;
                return std::abs(count - std::round(count)) <= tolerance;
            }
        }
        return true;
    }
    // Order-preserving integer coordinates avoid signed subtraction/addition overflow.
    static std::uint64_t ordinal(T value) {
        if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            constexpr auto middle = std::uint64_t{1} << std::numeric_limits<T>::digits;
            return value < 0 ? std::uint64_t(value - (std::numeric_limits<T>::lowest)())
                             : middle + std::uint64_t(value);
        } else return std::uint64_t(value);
    }
    static T fromOrdinal(std::uint64_t value) {
        if constexpr (std::is_integral_v<T> && std::is_signed_v<T>) {
            constexpr auto middle = std::uint64_t{1} << std::numeric_limits<T>::digits;
            return value < middle ? (std::numeric_limits<T>::lowest)() + T(value) : T(value - middle);
        } else return T(value);
    }
    T value_;
    std::optional<T> minimum_, maximum_, increment_;
};

} // namespace framegrabber::presentation
