#include "dragonet.h"

#include <lcm/lcm-cpp.hpp>

#include <sys/ioctl.h>
#include <sys/file.h>
#include <linux/rpmsg.h>
#include <fcntl.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <csignal>
#include <string.h>
#include <string>
#include <map>
#include <functional>
#include <vector>

#include <errno.h>

namespace dragonet {

#define MAX_EVENTS 100
#define RPMSG_ADDR_ANY 0xFFFFFFFF

class DragonetImpl
{
public:
    DragonetImpl()
    {
        signal(SIGPIPE, SIG_IGN);
        epoll_fd_ = epoll_create1(0);
        if (!lcm_.good())
        {
            fprintf(stderr, "[WARNING] LCM initialization error, disabling LCM\r\n");
            use_lcm_ = false;
        }
        else
        {
            fd_callbacks_[lcm_.getFileno()] = std::bind(&lcm::LCM::handle, &lcm_);
            addFileDescriptorToEpoll(lcm_.getFileno());
        }
        struct stat stat_buf;
        if (stat("/dev/rpmsg_ctrl0", &stat_buf) != 0)
        {
            fprintf(stderr, "[WARNING] rpmsg_char driver not loaded, not using RPMsg communication\r\n");
            use_rpmsg_ = false;
        }
    }

    void RegisterSubscription(const char *channel, std::function<void(char*)> callback, int msg_size, int queue_size)
    {
        if (use_lcm_)
        {
            lcm_callbacks_[std::string(channel)] = callback;
            lcm_.subscribeFunction(std::string(channel), lcmCallback, this);
        }

        if (use_rpmsg_)
        {
            struct rpmsg_endpoint_info info;
            sprintf(info.name, "%s__s", channel);
            info.src = RPMSG_ADDR_ANY;
            info.dst = RPMSG_ADDR_ANY;
            int dev_num = createEptDev(&info);
            if (dev_num == -1)
            {
                return;
            }
            char dev_path[24];
            sprintf(dev_path, "/dev/rpmsg%d", dev_num);
            int fd = open(dev_path, O_RDONLY);
            fd_callbacks_[fd] = [=]() {
                char buf[MAX_MESSAGE_SIZE];
                int len = read(fd, (char*) buf, msg_size);
                if (len == msg_size)
                {
                    callback((char*) buf);
                }
            };
            addFileDescriptorToEpoll(fd);
        }
    }

    void PublishMessage(const char *channel, const char *msg, int size)
    {
        // Intra-core subscribers
        if (use_lcm_)
        {
            lcm_.publish(std::string(channel), msg, size);
        }

        // Inter-core subscribers
        if (use_rpmsg_)
        {
            if (publisher_fds_.find(std::string(channel)) == publisher_fds_.end())
            {
                struct rpmsg_endpoint_info info;
                sprintf(info.name, "%s__p", channel);
                info.src = RPMSG_ADDR_ANY;
                info.dst = RPMSG_ADDR_ANY;
                int dev_num = createEptDev(&info);
                if (dev_num == -1)
                {
                    return;
                }
                char dev_path[24];
                sprintf(dev_path, "/dev/rpmsg%d", dev_num);
                int fd = open(dev_path, O_WRONLY);
                publisher_fds_[std::string(channel)] = fd;
            }
            write(publisher_fds_[std::string(channel)], msg, size);
        }
    }

    void DispatchCallbacks()
    {
        struct epoll_event events[MAX_EVENTS];
        int event_count = epoll_wait(epoll_fd_, events, MAX_EVENTS, 30000);
        for(int i = 0; i < event_count; i++)
        {
            fd_callbacks_[events[i].data.fd]();
        }
    }

private:
    int createEptDev(rpmsg_endpoint_info *info)
    {
        int fd = open("/dev/rpmsg_ctrl0", O_RDWR);
        if (fd == -1)
        {
            return -1;
        }
        flock(fd, LOCK_EX);
        ioctl(fd, RPMSG_CREATE_EPT_IOCTL, info);
        int dev_num = findEptDevByName(info->name);
        close(fd);
        return dev_num;
    }

    int findEptDevByName(const char *name)
    {
        DIR *sys_class_dir;
        if ((sys_class_dir = opendir("/sys/class/rpmsg")) == NULL)
        {
            return -1;
        }
        struct dirent *dev_ent;
        int highest_dev = 0;
        while ((dev_ent = readdir(sys_class_dir)) != NULL)
        {
            if (dev_ent->d_type == DT_LNK)
            {
                int dev_ent_num = atoi(&dev_ent->d_name[5]);
                if (dev_ent_num > highest_dev)
                {
                    highest_dev = dev_ent_num;
                }
            }
        }
        closedir(sys_class_dir);
        int dev_num = -1;
        for (int i = highest_dev; i >= 0; i--)
        {
            char name_path[40];
            sprintf(name_path, "/sys/class/rpmsg/rpmsg%d/name", i);
            int name_fd;
            if ((name_fd = open(name_path, O_RDONLY)) != -1)
            {
                char cur_name[33];
                read(name_fd, cur_name, 32);
                *(strchr(cur_name, '\n')) = '\0';
                if (strcmp(cur_name, name) == 0)
                {
                    dev_num = i;
                    close(name_fd);
                    break;
                }
                close(name_fd);
            }
        }
        return dev_num;
    }

    void addFileDescriptorToEpoll(int fd)
    {
        struct epoll_event event;
        event.events = EPOLLIN;
        event.data.fd = fd;
        int res = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &event);
    }

    static void lcmCallback(const lcm::ReceiveBuffer* rbuf, const std::string& channel, DragonetImpl *impl_instance)
    {
        impl_instance->lcm_callbacks_[channel]((char*) rbuf->data);
    };

    lcm::LCM lcm_;
    std::map<int, std::function<void(void)>> fd_callbacks_;
    std::map<std::string, int> publisher_fds_;
    std::map<std::string, std::function<void(char*)>> lcm_callbacks_;
    int epoll_fd_;
    bool use_lcm_{true};
    bool use_rpmsg_{true};
};

Dragonet::Dragonet()
{
}

Dragonet::~Dragonet() = default;

void Dragonet::Init()
{
    if (!initialized_)
    {
        impl_.reset(new DragonetImpl());
        initialized_ = true;
    }
}

void Dragonet::Spin()
{
    while (true)
    {
        impl_->DispatchCallbacks();
    }
}

int Dragonet::serializeAndPublish(const char *channel, const char *msg, int size)
{
    impl_->PublishMessage(channel, (char*) msg, size);
    return 0;
}

void Dragonet::subscribeSerialized(const char *channel, std::function<void(char*)> callback, int msg_size, int queue_size)
{
    impl_->RegisterSubscription(channel, callback, msg_size, queue_size);
}

}
