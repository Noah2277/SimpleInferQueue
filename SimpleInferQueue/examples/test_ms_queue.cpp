#include <iostream>
#include <thread>
#include <vector>
#include <atomic>
#include <mutex>
#include <set>
#include "ms_queue.h"

int main() {
    constexpr int PRODUCERS = 4;
    constexpr int CONSUMERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 1000;
    constexpr int TOTAL_ITEMS = PRODUCERS * ITEMS_PER_PRODUCER;

    MSQueue<int> q;
    std::atomic<int> produced_count(0);
    std::atomic<int> consumed_count(0);

    //用于验证不重复，不遗漏
    std::mutex mtx;
    std::set<int> consumed_values;

    //生产者线程
    std::vector<std::thread> producers;
    for(int p = 0; p < PRODUCERS; p++) {
        producers.emplace_back([&, p]() {
            for(int i = 0; i < ITEMS_PER_PRODUCER; i++) {
                int value = p * ITEMS_PER_PRODUCER + i; //每个生产者生成独立区间的值
                q.Enqueue(value);
                produced_count.fetch_add(1);
            }
        });
    }

    //消费者线程
    std::vector<std::thread> consumers;
    for(int c = 0; c < CONSUMERS; c++) {
        consumers.emplace_back([&]() {
            while (consumed_count.load() < TOTAL_ITEMS) {
                int val;
                if( q.Dequeue(val)) {
                    std::lock_guard<std::mutex> lock(mtx);
                    consumed_values.insert(val);
                    consumed_count.fetch_add(1);
                } else {
                    //队列空，但是生产者可能还没有完全结束
                    if(produced_count.load() < TOTAL_ITEMS) {
                        std::this_thread::yield();
                    } else {
                        // 所有生产者都生产完了，但队列却空了：
                        // 剩余元素要么在队列里，要么刚被别的消费者取走
                        // 让出CPU继续等待，最终一定会被取完
                        std::this_thread::yield();
                    }
                }
            }
        });
    }

    //等待所有线程结束
    for(auto& t : producers) t.join();
    for(auto& t : consumers) t.join();

    //验证
    bool success = (consumed_values.size() == TOTAL_ITEMS);
    if(success) {
        for(int i = 0; i < TOTAL_ITEMS; i++) {
            if(consumed_values.find(i) == consumed_values.end()) {
                //缺了这个值→有丢失
                success = false;
                break;
            }
        }
    }

    std::cout << "MPCP test: " << (success ? "PASS" : "FAIL")  << std::endl;

    return 0;
}