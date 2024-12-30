#include<iostream>
#include<vector>
#include<fstream>
#include<string>
#include<windows.h>
#include<chrono>
#include<map>
#include<mutex>
#include<algorithm>
#include<thread>

std::mutex resultmtx;


struct SearchResult {
    std::map<std::string, int> keywordCounts; // 记录每个关键字的词频
    std::map<std::string, int> start_time; // 记录每个关键字搜索时间
    std::map<std::string, int> end_time;
};


// 1、内存映射读入文件
class MappedFile {
public:
    MappedFile(const std::string& filename) {
        hFile_ = CreateFileA(filename.c_str(), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile_ == INVALID_HANDLE_VALUE) {
            std::cerr << "Failed to open file.\n";
            return;
        }

        LARGE_INTEGER size;
        if (!GetFileSizeEx(hFile_, &size)) {
            std::cerr << "Failed to get file size.\n";
            CloseHandle(hFile_);
            hFile_ = INVALID_HANDLE_VALUE;
            return;
        }
        fileSize_ = static_cast<size_t>(size.QuadPart);

        hMapping_ = CreateFileMapping(hFile_, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!hMapping_) {
            std::cerr << "Failed to create file mapping.\n";
            CloseHandle(hFile_);
            hFile_ = INVALID_HANDLE_VALUE;
            return;
        }

        data_ = MapViewOfFile(hMapping_, FILE_MAP_READ, 0, 0, 0);
        if (!data_) {
            std::cerr << "Failed to map view of file.\n";
            CloseHandle(hMapping_);
            CloseHandle(hFile_);
            hFile_ = INVALID_HANDLE_VALUE;
            return;
        }
    }

    ~MappedFile() {
        if (data_) {
            UnmapViewOfFile(data_);
        }
        if (hMapping_) {
            CloseHandle(hMapping_);
        }
        if (hFile_ != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile_);
        }
    }

    const char* data() const { return static_cast<const char*>(data_); }
    size_t size() const { return fileSize_; }
    bool isValid() const { return data_ != nullptr; }

private:
    HANDLE hFile_ = INVALID_HANDLE_VALUE;
    HANDLE hMapping_ = nullptr;
    size_t fileSize_ = 0;
    void* data_ = nullptr;
};

// kmp搜索算法
std::vector<int> get_next(std::string pattern) {
    std::vector<int> next;
    next.push_back(0);
    for (int i = 1, j = 0; i < pattern.length(); i++) {
        while (j > 0 && pattern[j] != pattern[i])
            j = next[j - 1];
        if (pattern[i] == pattern[j])
            j++;
        next.push_back(j);
    }
    return next;
}

int kmpsearch(const char* data, size_t datalen, std::string pattern) {
    int count = 0;
    std::vector<int> next = get_next(pattern);
    for (int i = 0, j = 0; i < datalen; i++) {
        while (j > 0 && data[i] != pattern[j]) j = next[j - 1];
        if (data[i] == pattern[j]) j++;
        if (j == pattern.length()) {
            count++;
            j = next[j - 1];
        }
    }
    return count;
}

// 2、单个线程的搜索任务
void searchInBlock(const char* data, size_t start, size_t end, const std::vector<std::string>& keywords, SearchResult& result) {
    std::map<std::string, int> blockcounts;// 该块中各个关键字的词频
    std::map<std::string, int> st; // 该块中各个关键字的用时
    std::map<std::string, int> et;
    // 开始搜索
    for (const auto& keyword : keywords) {
        size_t keywordlen = keyword.size();
        auto stime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());
        blockcounts[keyword] = kmpsearch(data + start, end - start, keyword);
        auto etime = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch());
        st[keyword] = stime.count();
        et[keyword] = etime.count();
    }
    // 合并结果
    std::unique_lock<std::mutex> lock(resultmtx);
    for (auto keyword : keywords) {
        result.keywordCounts[keyword] += blockcounts[keyword];
        if (result.start_time[keyword] == 0) result.start_time[keyword] = st[keyword];
        else result.start_time[keyword] = min(result.start_time[keyword], st[keyword]);
        if (result.end_time[keyword] == 0) result.end_time[keyword] = et[keyword];
        else result.end_time[keyword] = max(result.end_time[keyword], et[keyword]);
    }
}

int main() {
    const std::string filename = "enwiki-20231120-abstract1.xml";
    MappedFile mappedFile(filename);
    if (!mappedFile.isValid()) {
        std::cerr << "faile to map file." << std::endl;
        exit(0);
    }
    // 关键字文件读取
    std::ifstream keywordfile("keyword.txt");
    std::vector<std::string> keywords;
    std::string keyword;
    while (std::getline(keywordfile, keyword)) {
        keywords.push_back(keyword);
    }
    int threadcount = std::thread::hardware_concurrency(); // 线程数
    int overlapsize = 50; // 防止跨行匹配不到
    size_t blockSize = mappedFile.size() / threadcount;
    SearchResult result;
    std::vector<std::thread> threads;
    // 为每个块创建线程
    for (int i = 0; i < threadcount; ++i) {
        size_t start = i * blockSize;
        size_t end = (i == threadcount - 1) ? mappedFile.size() : (i + 1) * blockSize + overlapsize;
        // end = std::min(end, mappedFile.size()); // 防止越界
        threads.emplace_back(searchInBlock, mappedFile.data(), start, end, std::cref(keywords), std::ref(result));

    }
    // 执行线程
    for (auto& t : threads) t.join();
    for (auto kw : keywords) {
        std::cout << "key: " << kw << std::endl;
        std::cout << "count times: " << result.keywordCounts[kw] << std::endl;
        std::cout << "duration: " << result.end_time[kw] - result.start_time[kw] << "ms" << std::endl;
    }

    std::ofstream outputfile("output.txt");
    for (auto keyword : keywords) {
        outputfile << "count: " << result.keywordCounts[keyword] << '\t' << "time: " << result.end_time[keyword] - result.start_time[keyword] << "ms" << std::endl;
    }

    return 0;
}