// 保存最近成功提交的层释放 fence，查询仅借用描述符
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
class DirectReleaseCache {
public:
    DirectReleaseCache() = default;
    DirectReleaseCache(const DirectReleaseCache &) = delete;
    DirectReleaseCache &operator=(const DirectReleaseCache &) = delete;
    ~DirectReleaseCache() { Reset(); }

    void Reset()
    {
        int old = fd_;
        fd_ = -1; layer_ = 0; published_ = false;
        if (old >= 0) close(old);
    }

    void Adopt(uint32_t layer, int fd)
    {
        // 上一帧的释放 fence 还没有被取走就被新提交替换：记一次释放失步
        if (fd_ >= 0 && layer_ && !published_) ++lost_;
        Reset(); layer_ = layer; fd_ = fd;
    }

    void ResetLayer(uint32_t layer)
    {
        if (layer_ == layer) Reset();
    }

    int Borrow(uint32_t &layer) const
    {
        layer = layer_; return fd_;
    }

    // 交付前校验描述符仍然属于本缓存：调用方可能已 close，
    // 复用后的文件号（例如 DRM fd）绝不能再作为释放 fence 交出。
    int Acquire(uint32_t &layer)
    {
        layer = layer_;
        if (fd_ >= 0 && fcntl(fd_, F_GETFD) < 0) {
            Reset(); layer = 0; return -1;
        }
        published_ = fd_ >= 0;
        return fd_;
    }

    bool Published() const { return published_; }
    unsigned Lost() const { return lost_; }

private:
    uint32_t layer_ = 0;
    int fd_ = -1;
    bool published_ = false;
    unsigned lost_ = 0;
};