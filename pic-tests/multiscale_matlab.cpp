#include "mex.h"
#include <stdio.h>
#include <vector>
#include <time.h>
#include <stdlib.h>
#include <math.h>
#include <algorithm>
#include <tuple>
#include <exception>
#include "minimalrowSolver.cpp"
#include "shieldingSolver.cpp"

using namespace std;

#define __EPS__ 1e-13
#define __Inf__ numeric_limits<double>::infinity()

int unbelievable = 10000;
bool useShield;

class DataNode {
private:
    double measure;
    vector<DataNode*> children;
    tuple<double, double> center;
    int index;
public:
    DataNode(double _measure, tuple<double, double> _center, int _index) {
        measure = _measure;
        center = _center;
        index = _index;
    }
    DataNode(double _measure, vector<DataNode*> _children, int _index) {
        measure = _measure;
        children = _children;
        double xsum = 0.0, ysum = 0.0;
        for (unsigned int i = 0; i < children.size(); ++i) {
            xsum += get<0>(_children[i]->center);
            ysum += get<1>(_children[i]->center);
        }
        center = make_tuple(xsum/children.size(), ysum/children.size());
        index = _index;
        if (_index > unbelievable) {
            throw exception();
        }
    }

    double getMeasure() const {
        return measure;
    }

    const vector<DataNode*>& getChildren() const {
        return children;
    }

    void print() {
        printf("DataNode %p, ", this);
        if (children.size() == 0) {
            printf("leaf with measure %f.\n", measure);
        } else {
            printf("internode with measure %f.\n\tChildren: ", measure);
            for (unsigned int i = 0; i < children.size(); ++i) {
                printf("%p ,", children[i]);
            }
            printf("\n");
        }
    }

    tuple<double, double> getCenter() const {
        return center;
    }

    int getIndex() const {
        return index;
    }
};

class Decomposition {
private:
    int level;
    vector<DataNode*> partition;
public:
    Decomposition(int _level, vector<DataNode*> _partition) {
        level = _level;
        partition = _partition;
    }

    void assignLevel(int _level) {
        level = _level;
    }

    void print() {
        printf("Level %d decomposition, with %zd partition(s).\n", level, partition.size());
        for (unsigned int i = 0; i < partition.size(); ++i) {
            partition[i]->print();
        }
    }

    const vector<DataNode*>& getDataNodes() const {
        return partition;
    }

    void invalidate() {
        for (auto iter = partition.begin(); iter != partition.end(); ++iter) {
            delete *iter;
            *iter = nullptr;
        }
    }
};

class DecompositionChain {
private:
    vector<Decomposition> chain;
public:
    DecompositionChain(const vector<vector<double>> &X, int res, int clusterSize) {
        // uses the 2-d square structure
        // start from scale J, the finest one; however we don't know J in advance, so we will assign that later.
        vector<DataNode*> dataNodes;
        dataNodes.reserve(res*res);
        for (int i = 0; i < res; ++i) {
            for (int j = 0; j < res; ++j) {
                // we visit the image X in column-first order, that (0, 0) -> (1, 0) -> ...
                dataNodes.push_back(new DataNode(X[j][i], make_tuple(j, i), j+i*res));
            }
        }
        chain.push_back(Decomposition(0, dataNodes));
        int scaledRes = res;

        while (scaledRes != 1) {
            vector<int> partitionInIndex;
            if (scaledRes < 2*clusterSize) {
                partitionInIndex = vector<int>(1, scaledRes);
            } else {
                partitionInIndex = vector<int>(std::max(0, scaledRes/clusterSize-1), clusterSize);
                partitionInIndex.push_back(scaledRes%clusterSize+clusterSize);
            }
            vector<DataNode*> parentDataNodes;
            parentDataNodes.reserve(partitionInIndex.size()*partitionInIndex.size());

            for (unsigned int i = 0; i < partitionInIndex.size(); ++i) {
                for (unsigned int j = 0; j < partitionInIndex.size(); ++j) {
                    double mearsure = 0.0;
                    vector<DataNode*> children;
                    for (int k = 0; k < partitionInIndex[i]; ++k) {
                        for (int l = 0; l < partitionInIndex[j]; ++l) {
                            int index = j*clusterSize+l + (i*clusterSize+k)*scaledRes;
                            mearsure += dataNodes[index]->getMeasure();
                            children.push_back(dataNodes[index]);
                        }
                    }
                    parentDataNodes.push_back(new DataNode(mearsure, children, j+i*partitionInIndex.size()));
                }
            }

            dataNodes = parentDataNodes;
            scaledRes = partitionInIndex.size();
            chain.push_back(Decomposition(0, dataNodes));
        }

        // never forget to reverse the chain (because we build the finest one first)
        std::reverse(chain.begin(), chain.end());
        for (unsigned int i = 0; i < chain.size(); ++i) {
            chain[i].assignLevel(i);
        }
    }

    void invalidate() {
        for (auto iter = chain.begin(); iter != chain.end(); ++iter) {
            iter->invalidate();
        }
    }

    void print() {
        printf("%zd decomposition in chains:\n", chain.size());
        for (unsigned int i = 0; i < chain.size(); ++i) {
            chain[i].print();
        }
        printf("========================\n");
    }

    const vector<Decomposition>& getDecompositions() const {
        return chain;
    }
};

class Transport {
public:
    DataNode *x, *y;
    double amount;

    Transport(DataNode *_x, DataNode *_y, double _amount):x(_x), y(_y), amount(_amount) {}
};

class TransportPlan {
private:
    DecompositionChain *X, *Y;
    vector<vector<vector<double>>> *costChain;
    vector<vector<double>> cost;
    int level;
    vector<Transport> transports;

    TransportPlan(vector<vector<vector<double>>> *_costChain, DecompositionChain *_X, DecompositionChain *_Y, int _level) {
        X = _X;
        Y = _Y;
        costChain = _costChain;
        level = _level;
    }

    static auto search(const vector<vector<double>>& c, auto xIter, const vector<DataNode*>& yChildren) {
        int xIndex = (*xIter)->getIndex();
        double minimum = __Inf__;
        auto savedIter = yChildren.begin();
        for (auto iter = yChildren.begin(); iter != yChildren.end(); ++iter) {
            if (c[xIndex][(*iter)->getIndex()] < minimum) {
                minimum = c[xIndex][(*iter)->getIndex()];
                savedIter = iter;
            }
        }
        return *savedIter;
    }

    static void setToInf(vector<vector<double>>& c, bool opOnX, int index) {
        if (opOnX) {
            for (unsigned int i = 0; i < c[index].size(); ++i) {
                c[index][i] = __Inf__;
            }
        } else {
            for (unsigned int i = 0; i < c.size(); ++i) {
                c[i][index] = __Inf__;
            }
        }
    }
public:
    TransportPlan(vector<vector<vector<double>>> *_costChain, DecompositionChain *_X, DecompositionChain *_Y) {
        X = _X;
        Y = _Y;
        costChain = _costChain;
        level = 0;
        transports.push_back(Transport(X->getDecompositions()[0].getDataNodes()[0], Y->getDecompositions()[0].getDataNodes()[0], X->getDecompositions()[0].getDataNodes()[0]->getMeasure()));
    }

    TransportPlan propagate() {
        // if (level > 1)
        //     return *this;
        TransportPlan newPlan = TransportPlan(costChain, X, Y, level+1);
        auto tmpXFineNodes = X->getDecompositions()[level+1].getDataNodes(),
            tmpYFineNodes = Y->getDecompositions()[level+1].getDataNodes();

        // set up working measure and cost
        vector<double> tmpXMeasure;
        tmpXMeasure.reserve(tmpXFineNodes.size());
        for (auto xfIter = tmpXFineNodes.begin(); xfIter != tmpXFineNodes.end(); ++xfIter) {
            tmpXMeasure.push_back((*xfIter)->getMeasure());
        }
        vector<double> tmpYMeasure;
        tmpYMeasure.reserve(tmpYFineNodes.size());
        for (auto yfIter = tmpYFineNodes.begin(); yfIter != tmpYFineNodes.end(); ++yfIter) {
            tmpYMeasure.push_back((*yfIter)->getMeasure());
        }
        vector<vector<double>> tmpCost = (*costChain)[level+1];

        // for every transport process in `transports`, dispense it into finer nodes.
        for (auto iterator = transports.begin(); iterator != transports.end(); ++iterator) {
            auto xCoarseNode = iterator->x, yCoraseNode = iterator->y;
            double tmpAmount = iterator->amount;
            auto xCoarseNodeChildIter = xCoarseNode->getChildren().begin();
            while (true) {
                while (tmpXMeasure[(*xCoarseNodeChildIter)->getIndex()] < __EPS__) {
                    ++xCoarseNodeChildIter;
                    if (xCoarseNodeChildIter == xCoarseNode->getChildren().end()) {
                        printf("Error!!!!");
                        throw exception();
                    }
                }
                // now the iterator points to a positive measure dataNode
                double xm = tmpXMeasure[(*xCoarseNodeChildIter)->getIndex()];

                auto bestYCoarseNodeChild = search(tmpCost, xCoarseNodeChildIter, yCoraseNode->getChildren());
                double ym = tmpYMeasure[bestYCoarseNodeChild->getIndex()];
                //count++;
                if (tmpAmount <= ym + __EPS__ && tmpAmount <= xm + __EPS__) {
                    // use up the transport amount! time to go
                    tmpXMeasure[(*xCoarseNodeChildIter)->getIndex()] -= tmpAmount;
                    tmpYMeasure[bestYCoarseNodeChild->getIndex()] -= tmpAmount;
                    newPlan.transports.push_back(Transport(*xCoarseNodeChildIter, bestYCoarseNodeChild, tmpAmount));
                    break;
                } else if (ym < xm) {
                    // we can keep on searching without update x
                    tmpAmount -= tmpYMeasure[bestYCoarseNodeChild->getIndex()];
                    tmpXMeasure[(*xCoarseNodeChildIter)->getIndex()] -= tmpYMeasure[bestYCoarseNodeChild->getIndex()];
                    tmpYMeasure[bestYCoarseNodeChild->getIndex()] = 0;
                    // invalidate this y node
                    setToInf(tmpCost, false, bestYCoarseNodeChild->getIndex());
                    newPlan.transports.push_back(Transport(*xCoarseNodeChildIter, bestYCoarseNodeChild, ym));
                } else {
                    // x node has fewer measure
                    tmpAmount -= tmpXMeasure[(*xCoarseNodeChildIter)->getIndex()];
                    tmpYMeasure[bestYCoarseNodeChild->getIndex()] -= tmpXMeasure[(*xCoarseNodeChildIter)->getIndex()];
                    tmpXMeasure[(*xCoarseNodeChildIter)->getIndex()] = 0;
                    // invalidate this x node
                    setToInf(tmpCost, true, (*xCoarseNodeChildIter)->getIndex());
                    newPlan.transports.push_back(Transport(*xCoarseNodeChildIter, bestYCoarseNodeChild, xm));
                }
            }
            //printf("This time we added %d transports.(%d, %d)\n", count, tmpXMeasure.size(), tmpYMeasure.size());
        }
        // newPlan.prune();
        return newPlan;
    }

    void refine() {
        auto tmpXFineNodes = X->getDecompositions()[level].getDataNodes(),
            tmpYFineNodes = Y->getDecompositions()[level].getDataNodes();
        vector<double> tmpXMeasure;
        tmpXMeasure.reserve(tmpXFineNodes.size());
        for (auto xfIter = tmpXFineNodes.begin(); xfIter != tmpXFineNodes.end(); ++xfIter) {
            tmpXMeasure.push_back((*xfIter)->getMeasure());
        }
        vector<double> tmpYMeasure;
        tmpYMeasure.reserve(tmpYFineNodes.size());
        for (auto yfIter = tmpYFineNodes.begin(); yfIter != tmpYFineNodes.end(); ++yfIter) {
            tmpYMeasure.push_back((*yfIter)->getMeasure());
        }
        vector<tuple<int, int, double>> interact;
        interact.reserve(transports.size());
        for (auto itr = transports.begin(); itr != transports.end(); ++itr) {
            interact.push_back(make_tuple(itr->x->getIndex(), itr->y->getIndex(), itr->amount));
        }
        if (useShield) {
            interact = shield::wrapper_s((*costChain)[level], tmpXMeasure, tmpYMeasure, interact);
        } else {
            interact = wrapper((*costChain)[level], tmpXMeasure, tmpYMeasure, interact);
        }
        // transports = vector<Transport>(interact.size());
        auto XdataNodes = X->getDecompositions()[level].getDataNodes(),
            YdataNodes = Y->getDecompositions()[level].getDataNodes();
        for (unsigned int i = 0; i < interact.size(); ++i) {
            transports[i] = Transport(XdataNodes[get<0>(interact[i])], YdataNodes[get<1>(interact[i])], get<2>(interact[i]));
        }
    }

    void print() {
        printf("Transport plan at level %d:\n", level);
        double sum = 0.0;
        for (auto transport = transports.begin(); transport != transports.end(); ++transport) {
            // printf("\tFrom % 2d to % 2d with %05f\n", transport->x->getIndex(), transport->y->getIndex(), transport->amount);
            sum += transport->amount;
        }
        printf("Sum: %f.\n", sum);
    }

    const vector<Transport>& getTransports() const {
        return transports;
    }

    void invalidate() {
        X->invalidate();
        Y->invalidate();
    }
};

vector<vector<vector<double>>> decomposeCost(const vector<vector<double>> &cost, const DecompositionChain &Xchain, const DecompositionChain &Ychain) {
    int depth = Xchain.getDecompositions().size();
    vector<vector<vector<double>>> costChain(depth);
    costChain[depth-1] = cost;

    // very naive estimate of the distance cost
    for (int l = depth-2; l >= 0; --l) {
        auto xDecompNodes = Xchain.getDecompositions()[l].getDataNodes(),
            yDecompNodes = Ychain.getDecompositions()[l].getDataNodes();
        costChain[l].resize(xDecompNodes.size());
        for (unsigned int i = 0; i < xDecompNodes.size(); ++i) {
            auto xNodeChildren = xDecompNodes[i]->getChildren();
            costChain[l][i].resize(yDecompNodes.size());
            for (unsigned int j = 0; j < yDecompNodes.size(); ++j) {
                auto yNodeChildren = yDecompNodes[j]->getChildren();
                double costSum = 0;
                for (auto xiter = xNodeChildren.begin(); xiter != xNodeChildren.end(); ++xiter) {
                    for (auto yiter = yNodeChildren.begin(); yiter != yNodeChildren.end(); ++yiter) {
                        costSum += costChain[l+1][(*xiter)->getIndex()][(*yiter)->getIndex()];
                    }
                }
                costChain[l][i][j] = costSum / xNodeChildren.size() / yNodeChildren.size();
            }
        }
    }

    return costChain;
}

vector<tuple<int, int, double>> core(const vector<vector<double>> &X, const vector<vector<double>> &Y, const vector<vector<double>> &cost, int res) {
    clock_t begin = clock();
    const int clusterSize = 2;
    DecompositionChain Xchain = DecompositionChain(X, res, clusterSize);
    DecompositionChain Ychain = DecompositionChain(Y, res, clusterSize);

    vector<vector<vector<double>>> costChain = decomposeCost(cost, Xchain, Ychain);
    printf("Decomposition completed in %.4fs.\n", double(clock() - begin) / CLOCKS_PER_SEC);

    TransportPlan plan(&costChain, &Xchain, &Ychain);

    clock_t last = clock();
    for (unsigned i = 0; i < Xchain.getDecompositions().size()-1; ++i) {
        plan = plan.propagate();
        plan.refine();
        printf("Level %d completed in %.4fs (total %.4fs).\n", i+1, double(clock() - last) / CLOCKS_PER_SEC, double(clock() - begin) / CLOCKS_PER_SEC);
        last = clock();
    }
    vector<tuple<int, int, double>> result;
    auto transports = plan.getTransports();
    result.reserve(transports.size());
    for (unsigned int i = 0; i < transports.size(); ++i) {
        result.push_back(make_tuple(transports[i].x->getIndex(), transports[i].y->getIndex(), transports[i].amount));
    }
    plan.invalidate();
    return result;
}

void mexFunction(int nlhs, mxArray *plhs[], int nrhs, const mxArray *prhs[]) {
    printf("================================\nStarting multiscale method.\n");
    if (nrhs != 5) {
        mexErrMsgIdAndTxt("CTransimplex:gateway:nrhs", "Need five inputs!");
        return;
    }
    if (nlhs != 2) {
        mexErrMsgIdAndTxt("CTransimplex:gateway:nlhs", "Need two outputs!");
        return;
    }
    
    // read in data
    // cost matrix
    int m = mxGetM(prhs[1]);
    int n = mxGetN(prhs[1]);
    unbelievable = m;
    int res = sqrt(m);
    if (m != res*res) {
        mexErrMsgIdAndTxt("CTransimplex:gateway:res", "This solver can only deal with square inputs!");
        return;
    }
    vector<vector<double>> cost = vector<vector<double>>(m);
    double* costRe = mxGetPr(prhs[1]);
    // mu
    double* muRe = mxGetPr(prhs[2]);
    if (m != mxGetM(prhs[2])) {
        mexErrMsgIdAndTxt("CTransimplex:gateway:mu", "Mu dimension not match with cost matrix!");
        return;
    }
    vector<double> mu = vector<double>(m);
    // nu
    double* nuRe = mxGetPr(prhs[3]);
    if (n != mxGetM(prhs[3])) {
        mexErrMsgIdAndTxt("CTransimplex:gateway:nu", "Nu dimension not match with cost matrix!");
        return;
    }

    bool* logicals = mxGetLogicals(prhs[4]);
    useShield = logicals[0];

    printf("%s shielding.\n", useShield ? "Using" : "Not using");

    vector<double> nu = vector<double>(n);

    for (int i = 0; i < m; ++i) {
        cost[i].resize(n);
        for (int j = 0; j < n; ++j) {
            cost[i][j] = costRe[m*j+i];
        }
    }
    
    for (int i = 0; i < m; ++i) {
        mu[i] = muRe[i];
    }
    
    for (int j = 0; j < n; ++j) {
        nu[j] = nuRe[j];
    }

    vector<vector<double>> X = vector<vector<double>>(res);
    for (int i = 0; i < res; ++i) {
        X[i].resize(res);
        for (int j = 0; j < res; ++j) {
            X[i][j] = mu[res*j+i];
        }
    }

    vector<vector<double>> Y = vector<vector<double>>(res);
    for (int i = 0; i < res; ++i) {
        Y[i].resize(res);
        for (int j = 0; j < res; ++j) {
            Y[i][j] = nu[res*j+i];
        }
    }

    auto result = core(X, Y, cost, res);
    
    plhs[0] = mxCreateNumericMatrix(2, result.size(), mxINT32_CLASS, mxREAL);
    int32_t* val1 = (int32_t*) mxGetData(plhs[0]);
    for (unsigned int i = 0; i < result.size(); ++i) {
        val1[2*i] = get<0>(result[i]);
        val1[2*i+1] = get<1>(result[i]);
    }
    plhs[1] = mxCreateDoubleMatrix(1, result.size(), mxREAL);
    double* val2 = mxGetPr(plhs[1]);
    for (unsigned int i = 0; i < result.size(); ++i) {
        val2[i] = get<2>(result[i]);
    }

    return;
}