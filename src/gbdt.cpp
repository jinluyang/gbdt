#include <iostream>
#include <limits>
#include <numeric>
#include <algorithm>
#include <thread>
#include <omp.h>

#include "gbdt.h"
#include "timer.h"
/*
    本代码实现的是论文Greedy Function Approximation: A Gradient Boosting Machine
    Algorithm 5, l2_treeboost,
    所有计算公式均参考

*/
namespace {

//计算bias，公式在17行
float calc_bias(std::vector<float> const &Y)
{
//        std::cout << "ybar" << std::endl;
    double y_bar = std::accumulate(Y.begin(), Y.end(), 0.0);
//        std::cout << y_bar << std::endl;
    y_bar /= static_cast<double>(Y.size());
 //       std::cout << y_bar << std::endl;
    return static_cast<float>(log((1.0+y_bar)/(1.0-y_bar)));
}

//为每条样本记录树中的位置，以及残差，是否收缩
//shrinkage作用：不完全信任每一棵树，累加的时候以一定的权重只累加一部分
struct Location
{
    Location() : tnode_idx(1), r(0), shrinked(false) {}
    uint32_t tnode_idx;
    float r;
    bool shrinked;
};

struct Meta
{
    Meta() : sl(0), s(0), nl(0), n(0), v(0.0f/0.0f) {}
    double sl, s;
    uint32_t nl, n;
    float v;
};
//应该是分裂出的节点，ese是分裂值
struct Defender
{
    Defender() : ese(0), threshold(0) {}
    double ese;
    float threshold;
};

void scan(
    Problem const &prob,
    std::vector<Location> const &locations,
    std::vector<Meta> const &metas0,
    std::vector<Defender> &defenders,
    uint32_t const offset,// first
    bool  forward)
{   //nr_field字段数，nr_instance样本数
    uint32_t const nr_field = prob.nr_field;
    uint32_t const nr_instance = prob.nr_instance;

    #pragma omp parallel for schedule(dynamic)
    for(uint32_t j = 0; j < nr_field; ++j)
    {
        std::vector<Meta> metas = metas0;

        for(uint32_t i_bar = 0; i_bar < nr_instance; ++i_bar)
        {
            uint32_t const i = forward? i_bar : nr_instance-i_bar-1;

            Node const &dnode = prob.X[j][i];
            Location const &location = locations[dnode.i];
            if(location.shrinked)
                continue;

            //
            uint32_t const f = location.tnode_idx-offset;
            Meta &meta = metas[f];

            if(dnode.v != meta.v)
            {
                double const sr = meta.s - meta.sl;
                uint32_t const nr = meta.n - meta.nl;
                //当前节点误差=左子树均方误差+右子树均方误差
                double const current_ese =
                    (meta.sl*meta.sl)/static_cast<double>(meta.nl) +
                    (sr*sr)/static_cast<double>(nr);

                Defender &defender = defenders[f*nr_field+j];
                double &best_ese = defender.ese;
                if(current_ese > best_ese)
                {
                    best_ese = current_ese;
                    defender.threshold = forward? dnode.v : meta.v;
                }
                if(i_bar > nr_instance/2)
                    break;
            }

            meta.sl += location.r;
            ++meta.nl;
            meta.v = dnode.v;
        }
    }
}
/*
void scan_sparse(
    Problem const &prob,
    std::vector<Location> const &locations,
    std::vector<Meta> const &metas0,
    std::vector<Defender> &defenders,
    uint32_t const offset,
    bool const forward)
{
    uint32_t const nr_sparse_field = prob.nr_sparse_field;
    uint32_t const nr_leaf = offset;

    #pragma omp parallel for schedule(dynamic)
    for(uint32_t j = 0; j < nr_sparse_field; ++j)
    {
        std::vector<Meta> metas = metas0;
        for(uint64_t p = prob.SIP[j]; p < prob.SIP[j+1]; ++p)
        {
            Location const &location = locations[prob.SI[p]];
            if(location.shrinked)
                continue;
            Meta &meta = metas[location.tnode_idx-offset];
            meta.sl += location.r;
            ++meta.nl;
        }

        for(uint32_t f = 0; f < nr_leaf; ++f)
        {
            Meta const &meta = metas[f];
            if(meta.nl == 0)
                continue;

            double const sr = meta.s - meta.sl;
            uint32_t const nr = meta.n - meta.nl;
            double const current_ese =
                (meta.sl*meta.sl)/static_cast<double>(meta.nl) +
                (sr*sr)/static_cast<double>(nr);

            Defender &defender = defenders[f*nr_sparse_field+j];
            double &best_ese = defender.ese;
            if(current_ese > best_ese)
            {
                best_ese = current_ese;
                defender.threshold = 1;
            }
        }
    }
}*/

} //unnamed namespace

uint32_t CART::max_depth = 7;
uint32_t CART::max_tnodes = static_cast<uint32_t>(pow(2, CART::max_depth+1));
std::mutex CART::mtx;
bool CART::verbose = false;

void CART::fit(Problem const &prob, std::vector<float> const &R,
    std::vector<float> &F1)
{
    uint32_t const nr_field = prob.nr_field;
//    uint32_t const nr_sparse_field = prob.nr_sparse_field;
    uint32_t const nr_instance = prob.nr_instance;

    //记录样本的位置
    std::vector<Location> locations(nr_instance);
    #pragma omp parallel for schedule(static)
    //初始化残差
    for(uint32_t i = 0; i < nr_instance; ++i)
        locations[i].r = R[i];
    //按层遍历
    for(uint32_t d = 0, offset = 1; d < max_depth; ++d, offset *= 2)
    {
        //每层的节点数为2^d
        uint32_t const nr_leaf = static_cast<uint32_t>(pow(2, d));
        std::vector<Meta> metas0(nr_leaf);

        for(uint32_t i = 0; i < nr_instance; ++i)
        {
            Location &location = locations[i];
            if(location.shrinked)
                continue;
            //每个节点距离本层首节点的偏移
            Meta &meta = metas0[location.tnode_idx-offset];
            //累加残差，为后面的计算公式做准备
            meta.s += location.r;
            ++meta.n;
        }

        //每层节点数*字段数
        std::vector<Defender> defenders(nr_leaf*nr_field);
//        std::vector<Defender> defenders_sparse(nr_leaf*nr_sparse_field);
        for(uint32_t f = 0; f < nr_leaf; ++f)
        {
            Meta const &meta = metas0[f];
            double const ese = meta.s*meta.s/static_cast<double>(meta.n);
            for(uint32_t j = 0; j < nr_field; ++j)
                defenders[f*nr_field+j].ese = ese;
//            for(uint32_t j = 0; j < nr_sparse_field; ++j)
//                defenders_sparse[f*nr_sparse_field+j].ese = ese;
        }
        std::vector<Defender> defenders_inv = defenders;

        std::thread thread_f(scan, std::ref(prob), std::ref(locations),
            std::ref(metas0), std::ref(defenders), offset, true);
        std::thread thread_b(scan, std::ref(prob), std::ref(locations),
            std::ref(metas0), std::ref(defenders_inv), offset, false);
//        scan_sparse(prob, locations, metas0, defenders_sparse, offset, true);
        thread_f.join();
        thread_b.join();

        //对每个节点计算
        for(uint32_t f = 0; f < nr_leaf; ++f)
        {
            Meta const &meta = metas0[f];
            double best_ese = meta.s*meta.s/static_cast<double>(meta.n);
            TreeNode &tnode = tnodes[f+offset];
            //计算每个字段的最佳分裂属性和分裂值
            for(uint32_t j = 0; j < nr_field; ++j)
            {
                Defender defender = defenders[f*nr_field+j];
                if(defender.ese > best_ese)
                {
                    best_ese = defender.ese;
                    tnode.feature = j;
                    tnode.threshold = defender.threshold;
                }

                defender = defenders_inv[f*nr_field+j];
                if(defender.ese > best_ese)
                {
                    best_ese = defender.ese;
                    tnode.feature = j;
                    tnode.threshold = defender.threshold;
                }
            }
//            for(uint32_t j = 0; j < nr_sparse_field; ++j)
//            {
//                Defender defender = defenders_sparse[f*nr_sparse_field+j];
//                if(defender.ese > best_ese)
//                {
//                    best_ese = defender.ese;
//                    tnode.feature = nr_field + j;
//                    tnode.threshold = defender.threshold;
//                }
//            }
        }
        //将样本分配到节点中
        #pragma omp parallel for schedule(static)
        for(uint32_t i = 0; i < nr_instance; ++i)
        {
            Location &location = locations[i];
            if(location.shrinked)
                continue;

            uint32_t &tnode_idx = location.tnode_idx;
            TreeNode &tnode = tnodes[tnode_idx];
            //已到叶子节点
            if(tnode.feature == -1)
            {
                location.shrinked = true;
            }
            //分到左或右节点，结果直接保存到location中
            else if(static_cast<uint32_t>(tnode.feature) < nr_field)
            {
                if(prob.Z[tnode.feature][i].v < tnode.threshold)
                    tnode_idx = 2*tnode_idx;
                else
                    tnode_idx = 2*tnode_idx+1;
            }
            //稀疏字段
            else
            {
                uint32_t const target_feature
                    = static_cast<uint32_t>(tnode.feature-nr_field);
                bool is_one = false;
                for(uint64_t p = prob.SJP[i]; p < prob.SJP[i+1]; ++p)
                {
                    if(prob.SJ[p] == target_feature)
                    {
                        is_one = true;
                        break;
                    }
                }
                if(!is_one)
                    tnode_idx = 2*tnode_idx;
                else
                    tnode_idx = 2*tnode_idx+1;
            }
        }
    }

    std::vector<std::pair<double, double>>
        tmp(max_tnodes, std::make_pair(0, 0));

    //first是残差
    //second为计算gamma做准备
    for(uint32_t i = 0; i < nr_instance; ++i)
    {
        float const r = locations[i].r;
        uint32_t const tnode_idx = locations[i].tnode_idx;
        tmp[tnode_idx].first += r;
        tmp[tnode_idx].second += fabs(r)*(1-fabs(r));
    }

    //计算gamma
    for(uint32_t tnode_idx = 1; tnode_idx <= max_tnodes; ++tnode_idx)
    {
        double a, b;
        //first -> a, second -> b
        std::tie(a, b) = tmp[tnode_idx];
        tnodes[tnode_idx].gamma = (b <= 1e-12)? 0 : static_cast<float>(a/b);
    }

    #pragma omp parallel for schedule(static)
    for(uint32_t i = 0; i < nr_instance; ++i)
        F1[i] = tnodes[locations[i].tnode_idx].gamma;
}
//预测样本，并分到左右节点
std::pair<uint32_t, float> CART::predict(float const * const x) const
{
    uint32_t tnode_idx = 1;
    for(uint32_t d = 0; d <= max_depth; ++d)
    {
        TreeNode const &tnode = tnodes[tnode_idx];
        if(tnode.feature == -1)
            return std::make_pair(tnode.idx, tnode.gamma);

        if(x[tnode.feature] < tnode.threshold)
            tnode_idx = tnode_idx*2;
        else
            tnode_idx = tnode_idx*2+1;
    }

    return std::make_pair(-1, -1);
}

void GBDT::fit(Problem const &Tr, Problem const &Va)
{
    bias = calc_bias(Tr.Y);
//        std::cout << bias << std::endl;

    std::vector<float> F_Tr(Tr.nr_instance, bias), F_Va(Va.nr_instance, bias);

    Timer timer;
    printf("iter     time    tr_loss    va_loss\n");
    for(uint32_t t = 0; t < trees.size(); ++t)
    {
        timer.tic();

        std::vector<float> const &Y = Tr.Y;
        std::vector<float> R(Tr.nr_instance), F1(Tr.nr_instance);

        #pragma omp parallel for schedule(static)
        //初始化残差，计算公式参考论文
        for(uint32_t i = 0; i < Tr.nr_instance; ++i)
            R[i] = static_cast<float>(Y[i]/(1+exp(Y[i]*F_Tr[i])));

        trees[t].fit(Tr, R, F1);

        double Tr_loss = 0;
        #pragma omp parallel for schedule(static) reduction(+: Tr_loss)
        //log loss
        //Y为真实值，f_tr为训练值
        for(uint32_t i = 0; i < Tr.nr_instance; ++i)
        {
            F_Tr[i] += F1[i];
            Tr_loss += log(1+exp(-Y[i]*F_Tr[i]));
        }
        Tr_loss /= static_cast<double>(Tr.nr_instance);
//        std::cout << "va nrinstance"<< Va.nr_instance << std::endl;

        #pragma omp parallel for schedule(static)
        for(uint32_t i = 0; i < Va.nr_instance; ++i)
        {
            std::vector<float> x = construct_instance(Va, i);
//        std::cout << i << std::endl;
            F_Va[i] += trees[t].predict(x.data()).second;
        }

        double Va_loss = 0;
 //       std::cout << F_Va[0] << std::endl;
        #pragma omp parallel for schedule(static) reduction(+: Va_loss)
        //validation误差
        for(uint32_t i = 0; i < Va.nr_instance; ++i)
            Va_loss += log(1+exp(-Va.Y[i]*F_Va[i]));
        Va_loss /= static_cast<double>(Va.nr_instance);
        std::cout << Va_loss << std::endl;

        printf("%4d %8.1f %10.5f %10.5f\n", t, timer.toc(), Tr_loss, Va_loss);
        fflush(stdout);
    }
}

float GBDT::predict(float const * const x) const
{
    float s = bias;
    for(auto &tree : trees)
        s += tree.predict(x).second;
    return s;
}

//获得索引
std::vector<uint32_t> GBDT::get_indices(float const * const x) const
{
    uint32_t const nr_tree = static_cast<uint32_t>(trees.size());

    std::vector<uint32_t> indices(nr_tree);
    for(uint32_t t = 0; t < nr_tree; ++t)
        indices[t] = trees[t].predict(x).first;
    return indices;
}
