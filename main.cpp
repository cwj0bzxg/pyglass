//
// Created by weijian on 5/13/24.
//

#include <iostream>
#include <vector>
#include <fstream>
#include <cassert>
#include <string>
#include <cstring>
#include <set>

#include "glass/searcher.hpp"
#include "glass/quant/fp32_quant.hpp"
#include "glass/hnsw/hnsw.hpp"
#include "glass/nsg/nsg.hpp"

using std::cout;
using std::endl;
using std::string;
using std::vector;

/// @brief Reading binary data vectors. Raw data store as a (N x dim)
/// @param file_path file path of binary data
/// @param data returned 2D data vectors
template<class T>
void ReadBin(const std::string &file_path,
             std::vector<std::vector<T>> &data) {
    std::cout << "Reading Data: " << file_path << std::endl;
    std::ifstream ifs;
    ifs.open(file_path, std::ios::binary);
    assert(ifs.is_open());
    unsigned N;  // num of points
    unsigned num_dimensions;
    ifs.read((char *)&N, sizeof(unsigned));
    ifs.read((char *)&num_dimensions, sizeof(unsigned));
    data.resize(N);
    std::cout << "# of points: " << N << std::endl;
    std::cout << "# of dimensions: " << num_dimensions << std::endl;
    std::vector<T> buff(num_dimensions);
    int counter = 0;
    for(int i = 0; i < N; i++) {
        ifs.read((char *)buff.data(), num_dimensions * sizeof(T));
        std::vector<T> row(num_dimensions);
        for (int d = 0; d < num_dimensions; d++) {
            row[d] = static_cast<T>(buff[d]);
        }
        data[counter++] = std::move(row);
    }
    ifs.close();
    std::cout << "Finish Reading Data" << endl;
}

void printFormattedVector(const std::vector<double>& vec) {
    std::cerr << "data = [";
    for (size_t i = 0; i < vec.size(); ++i) {
        std::cerr << vec[i];
        if (i < vec.size() - 1) {
            std::cerr << ", ";
        }
    }
    std::cerr << "]" << std::endl;
}


int main() {
    string data_file = "/data/raid0/deep1m/deep1m_base.fbin";
    string query_file = "/data/raid0/deep1m/deep1m_query.fbin";
    string gt_file = "/data/raid0/deep1m/deep1m_gt";
//    string data_file = "/data/raid0/deep10m/base.1B.fbin.crop_nb_10000000";
//    string query_file = "/data/raid0/deep10m/query.public.10K.fbin";
//    string gt_file = "/data/raid0/deep10m/deep-10M";

    bool update_index = false;

    vector<vector<float>> data{}, queries{};
    vector<vector<int>> GT{};
    vector<float> data_buf{};

    ReadBin(data_file, data);
    ReadBin(query_file, queries);
    ReadBin(gt_file, GT);

    int nb = data.size(), d = data.front().size();
    int nq = queries.size();
    int k = 10;
    vector<vector<int>> output(nq, vector<int>(k, 0));
    data_buf.resize(nb * d);
#pragma omp parallel for
    for (int i = 0; i < nb; i++) {
        std::memcpy(data_buf.data() + i * d, data[i].data(), d * 4);
    }

    glass::NSG index(d, "L2", 64, 100);
    if (update_index) {
        index.Build(data_buf.data(), nb);
        index.final_graph.save("hnsw_index_glass");
    } else {
        index.final_graph.load("hnsw_index_glass");
    }

    glass::Searcher<glass::FP32Quantizer<glass::Metric::L2>> searcher(index.final_graph);
    searcher.SetData(data_buf.data(), nb, d);
    std::vector<int> efs{10, 20, 30, 40, 50, 75, 100};
    for (auto ef : efs) {

        searcher.SetEf(ef);
//    searcher.Optimize(96);

        auto start = std::chrono::high_resolution_clock::now();
#pragma omp parallel for schedule(dynamic) num_threads(8)
        for (int i = 0; i < nq; i++) {
            searcher.Search(queries[i].data(), k, output[i].data());
        }
        auto end = std::chrono::high_resolution_clock::now();
        cout << "search time: " << std::chrono::duration<double>(end - start).count() << " s" << std::endl;

        std::atomic<int> total_coselection{0};
        std::atomic<int> total_num{0};
        std::vector<double> recalls{};
//#pragma omp parallel for
        for (int i = 0; i < nq; i++) {
            int cur_coselection = 0;
            std::set gt(GT[i].begin(), GT[i].end());
            std::set res(output[i].begin(), output[i].end());
            for (auto item: res) {
                if (gt.find(static_cast<int64_t>(item)) != gt.end()) {
                    cur_coselection++;
                }
            }
            total_num += 1;
            total_coselection += cur_coselection;
            recalls.push_back((double)cur_coselection * 100 / k);
        }
//        printFormattedVector(recalls);

        std::cout << "recall = " << (double) total_coselection * 100 / (total_num * k) << " %" << std::endl;
    }
}