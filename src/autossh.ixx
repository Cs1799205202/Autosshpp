export module autosshpp.autossh;

import std;
import autosshpp.common;
import autosshpp.config;

export namespace autosshpp {

class AutoSSH {
public:
    explicit AutoSSH(Config config);
    ~AutoSSH();

    AutoSSH(AutoSSH&&) noexcept;
    auto operator=(AutoSSH&&) noexcept -> AutoSSH&;

    AutoSSH(const AutoSSH&) = delete;
    auto operator=(const AutoSSH&) -> AutoSSH& = delete;

    [[nodiscard]] auto run() -> Result<int>;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace autosshpp
