#include "persistence/Database.hpp"

namespace persistence {

namespace {

constexpr std::string_view kBooleanTrue = "t";

}

std::size_t Rows::size() const noexcept { return result_ ? result_->size() : 0; }

std::size_t Rows::affected() const noexcept { return result_ ? result_->affectedRows() : 0; }

bool Rows::isNull(const std::size_t row, const std::size_t column) const noexcept {
    if (!result_ || row >= result_->size() || column >= result_->columns()) {
        return true;
    }
    return result_->at(row).at(column).isNull();
}

std::string Rows::text(const std::size_t row, const std::size_t column) const {
    if (isNull(row, column)) {
        return {};
    }
    return result_->at(row).at(column).as<std::string>();
}

std::optional<std::string> Rows::optionalText(const std::size_t row, const std::size_t column) const {
    if (isNull(row, column)) {
        return std::nullopt;
    }
    return text(row, column);
}

std::int64_t Rows::integer(const std::size_t row, const std::size_t column) const {
    if (isNull(row, column)) {
        return 0;
    }
    return result_->at(row).at(column).as<std::int64_t>();
}

bool Rows::boolean(const std::size_t row, const std::size_t column) const {
    if (isNull(row, column)) {
        return false;
    }
    return result_->at(row).at(column).as<std::string>().starts_with(kBooleanTrue);
}

security::Bytes Rows::bytes(const std::size_t row, const std::size_t column) const {
    if (isNull(row, column)) {
        return {};
    }
    const auto raw = result_->at(row).at(column).as<std::vector<char>>();
    return {raw.begin(), raw.end()};
}

std::chrono::system_clock::time_point Rows::time(const std::size_t row, const std::size_t column) const {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(integer(row, column)));
}

std::optional<std::chrono::system_clock::time_point> Rows::optionalTime(const std::size_t row,
                                                                        const std::size_t column) const {
    if (isNull(row, column)) {
        return std::nullopt;
    }
    return time(row, column);
}

}
