﻿#include "init.hpp"
#include "position.hpp"
#include "usi.hpp"
#include "move.hpp"
#include "generateMoves.hpp"
#include "search.hpp"
#include "book.hpp"
#include "fastmath.h"
#include <set>
#include <iostream>
#include <fstream>
#include <random>
#include <thread>
#include <mutex>
#include <memory>
#include <signal.h>

#include "Node.h"
#include "LruCache.h"
#include "mate.h"
#include "nn_tensorrt.h"
#include "dfpn.h"
#include "USIEngine.h"

#include "cppshogi.h"

#include "cxxopts/cxxopts.hpp"
constexpr double DYNAMIC_PARAM = 6.0;
//#define SPDLOG_TRACE_ON
#define SPDLOG_DEBUG_ON
#define SPDLOG_EOL "\n"
#include "spdlog/spdlog.h"
auto loggersink = std::make_shared<spdlog::sinks::stdout_sink_mt>();
auto logger = std::make_shared<spdlog::async_logger>("selfplay", loggersink, 8192);

using namespace std;

// 候補手の最大数(盤上全体)
constexpr int UCT_CHILD_MAX = 593;
int threads = 2;
const int INTERVAL = 150;
float ALPHA_D = 0.75f;
float KLDGAIN_THRESHOLD = 0.0000100;
void random_dirichlet(std::mt19937_64& mt, std::vector<float>& x, const float alphaSum) {

	float sum_y = 0;
	//std::gamma_distribution<float> gamma(0.15f, 1.0);
	for (int i = 0; i < x.size(); i++) {
		std::gamma_distribution<float> gamma(alphaSum * x[i], 1.0);
		float y = gamma(mt);
		sum_y += y;
		x[i] = y;
	}
	std::for_each(x.begin(), x.end(), [sum_y](float& v) { v /= sum_y; });
}
volatile sig_atomic_t stopflg = false;

void sigint_handler(int signum)
{
	stopflg = true;
}

// ランダムムーブの手数
int RANDOM_MOVE;
// 訪問数が最大のノードの価値の一定以下は除外
float RANDOM_CUTOFF = 0.015f;
// 1手ごとに低下する価値の閾値
float RANDOM_CUTOFF_DROP = 0.001f;
// 訪問数に応じてランダムに選択する際の温度パラメータ
float RANDOM_TEMPERATURE = 10.0f;
float RANDOM_TEMPERATURE_FINAL = 0.45f;
// 1手ごとに低下する温度
float RANDOM_TEMPERATURE_DROP = 1.0f;
// ランダムムーブした局面を学習する
bool TRAIN_RANDOM = false;
// 訪問回数が最大の手が2番目の手のx倍以内の場合にランダムに選択する
float RANDOM2 = 0;
// 出力する最低手数
int MIN_MOVE;
// ルートの方策に加えるノイズの確率(千分率)
int ROOT_NOISE;

int WINRATE_COUNT;

// 終局とする勝率の閾値
float WINRATE_THRESHOLD;

// 詰み探索の深さ
uint32_t ROOT_MATE_SEARCH_DEPTH;
// 詰み探索の最大ノード数
int64_t MATE_SEARCH_MAX_NODE;
constexpr int64_t MATE_SEARCH_MIN_NODE = 1000;

// モデルのパス
string model_path;

set<Key> st[1000];
int st_count[1000];

int playout_num = 1000;

// USIエンジンのパス
string usi_engine_path;
int usi_engine_num;
int usi_threads;
// USIエンジンオプション（name:value,...,name:value）
string usi_options;
int usi_byoyomi;
int usi_turn; // 0:先手、1:後手、それ以外:ランダム

std::mutex mutex_all_gpu;

int MAX_MOVE = 320; // 最大手数
bool OUT_MAX_MOVE = false; // 最大手数に達した対局の局面を出力するか
constexpr int EXTENSION_TIMES = 2; // 探索延長回数
bool REUSE_SUBTREE = false; // 探索済みノードを再利用するか

struct CachedNNRequest {
	CachedNNRequest(size_t size) : nnrate(size) {}
	float value_win;
	std::vector<float> nnrate;
};
typedef LruCache<uint64_t, CachedNNRequest> NNCache;
typedef LruCacheLock<uint64_t, CachedNNRequest> NNCacheLock;
unsigned int nn_cache_size = 8388608; // NNキャッシュサイズ

int search_param_black_wins[3][3];
int search_param_white_wins[3][3];

s64 teacherNodes; // 教師局面数
std::atomic<s64> idx(0);
std::atomic<s64> madeTeacherNodes(0);
std::atomic<s64> black_wins(0);
std::atomic<s64> white_wins(0);
std::atomic<s64> games(0);
std::atomic<s64> draws(0);
std::atomic<s64> nyugyokus(0);
// プレイアウト数
std::atomic<s64> sum_playouts(0);
double sum_c_dynamic = 0;
std::atomic<s64> sum_playouts_remove(0);
std::atomic<s64> sum_nodes(0);
// USIエンジンとの対局結果
std::atomic<s64> usi_games(0);
std::atomic<s64> usi_wins(0);
std::atomic<s64> usi_draws(0);

ifstream ifs;
ofstream ofs;
bool SPLIT_OPPONENT = false;
ofstream ofs_opponent;
bool OUT_MIN_HCP = false;
ofstream ofs_minhcp;
mutex imutex;
mutex omutex;
size_t entryNum;

// ランダム
uniform_int_distribution<int> rnd(0, 999);

// 末端ノードでの詰み探索の深さ(奇数であること)
#ifndef MATE_SEARCH_DEPTH
constexpr int MATE_SEARCH_DEPTH = 7;
#endif

// 探索の結果を評価のキューに追加したか
constexpr float QUEUING = FLT_MAX;

float c_init;
float c_base;
float c_fpu_reduction;
float c_init_root;
float c_base_root;
float temperature;
float root_temperature;

typedef pair<uct_node_t*, unsigned int> trajectory_t;
typedef vector<trajectory_t> trajectories_t;

struct visitor_t {
	trajectories_t trajectories;
	float value_win;
};

// バッチの要素
struct batch_element_t {
	uct_node_t* node;
	Color color;
	Key key;
	float* value_win;
};

// 探索結果の更新
inline void
UpdateResult(child_node_t* child, float result, uct_node_t* current)
{
	current->win += (WinType)result;
	current->win2 += (WinType)(result * result);
	current->move_count++;
	child->win += (WinType)result;
	child->win2 += (WinType)(result * result);
	child->move_count++;
}

bool compare_child_node_ptr_descending(const child_node_t* lhs, const child_node_t* rhs)
{
	if (lhs->IsWin()) {
		// 負けが確定しているノードは選択しない
		if (rhs->IsWin()) {
			// すべて負けの場合は、探索回数が最大の手を選択する
			if (lhs->move_count == rhs->move_count)
				return lhs->nnrate > rhs->nnrate;
			return lhs->move_count > rhs->move_count;
		}
		return false;
	}
	else if (lhs->IsLose()) {
		// 子ノードに一つでも負けがあれば、勝ちなので選択する
		if (rhs->IsLose()) {
			// すべて勝ちの場合は、探索回数が最大の手を選択する
			if (lhs->move_count == rhs->move_count)
				return lhs->nnrate > rhs->nnrate;
			return lhs->move_count > rhs->move_count;
		}
		return true;
	}
	else if (rhs->IsWin()) {
		// 負けが確定しているノードは選択しない
		if (lhs->IsWin()) {
			// すべて負けの場合は、探索回数が最大の手を選択する
			if (lhs->move_count == rhs->move_count)
				return lhs->nnrate > rhs->nnrate;
			return lhs->move_count > rhs->move_count;
		}
		return true;
	}
	else if (rhs->IsLose()) {
		// 子ノードに一つでも負けがあれば、勝ちなので選択する
		if (lhs->IsLose()) {
			// すべて勝ちの場合は、探索回数が最大の手を選択する
			if (lhs->move_count == rhs->move_count)
				return lhs->nnrate > rhs->nnrate;
			return lhs->move_count > rhs->move_count;
		}
		return false;
	}
	if (lhs->move_count == rhs->move_count)
		return lhs->nnrate > rhs->nnrate;
	return lhs->move_count > rhs->move_count;
}

// 価値(勝率)から評価値に変換
inline s16 value_to_score(const float value) {
	if (value == 1.0f)
		return 30000;
	else if (value == 0.0f)
		return -30000;
	else
		return s16(-logf(1.0f / value - 1.0f) * 756.0864962951762f);
}

// 詰み探索スロット
struct MateSearchEntry {
	Position* pos;
	enum State { RUNING, NOMATE, WIN, LOSE };
	atomic<State> status;
	Move move;
};

Searcher s;

class UCTSearcher;
class UCTSearcherGroupPair;
class UCTSearcherGroup {
public:
	UCTSearcherGroup(const int gpu_id, const int group_id, const int policy_value_batch_maxsize, UCTSearcherGroupPair* parent) :
		gpu_id(gpu_id), group_id(group_id), policy_value_batch_maxsize(policy_value_batch_maxsize), parent(parent),
		nn_cache(nn_cache_size),
		current_policy_value_batch_index(0), features1(nullptr), features2(nullptr), policy_value_batch(nullptr), y1(nullptr), y2(nullptr), running(false) {
		Initialize();
	}
	UCTSearcherGroup(UCTSearcherGroup&& o) {} // not use
	~UCTSearcherGroup() {
		delete[] policy_value_batch;
		delete[] mate_search_slot;
		checkCudaErrors(cudaFreeHost(features1));
		checkCudaErrors(cudaFreeHost(features2));
		checkCudaErrors(cudaFreeHost(y1));
		checkCudaErrors(cudaFreeHost(y2));
	}

	void QueuingNode(const Position* pos, uct_node_t* node, float* value_win);
	void EvalNode();
	void SelfPlay();
	void Run();
	void Join();
	void QueuingMateSearch(Position* pos, const int id) {
		lock_guard<mutex> lock(mate_search_mutex);
		mate_search_slot[id].pos = pos;
		mate_search_slot[id].status = MateSearchEntry::RUNING;
		mate_search_queue.push_back(id);
	}
	MateSearchEntry::State GetMateSearchStatus(const int id) {
		return mate_search_slot[id].status;
	}
	Move GetMateSearchMove(const int id) {
		return mate_search_slot[id].move;
	}
	void MateSearch();

	int group_id;
	int gpu_id;
	bool running;
	// USIEngine
	vector<USIEngine> usi_engines;

private:
	void Initialize();

	UCTSearcherGroupPair* parent;

	// NNキャッシュ
	NNCache nn_cache;

	// キュー
	int policy_value_batch_maxsize; // 最大バッチサイズ
	packed_features1_t* features1;
	packed_features2_t* features2;
	batch_element_t* policy_value_batch;
	int current_policy_value_batch_index;

	// UCTSearcher
	vector<UCTSearcher> searchers;
	thread* handle_selfplay;

	DType* y1;
	DType* y2;

	// 詰み探索
	DfPn dfpn;
	MateSearchEntry* mate_search_slot = nullptr;
	deque<int> mate_search_queue;
	mutex mate_search_mutex;
	thread* handle_mate_search = nullptr;
};
float search_param_diff[3] = { -0.08, 0.0, 0.08 };
mt19937_64 mt_64_global;
class UCTSearcher {
public:
	UCTSearcher(UCTSearcherGroup* grp, NNCache& nn_cache, const int id, const size_t entryNum) :
		grp(grp),
		nn_cache(nn_cache),
		id(id),
		mt_64(new std::mt19937_64(std::chrono::system_clock::now().time_since_epoch().count() + id)),
		mt(new std::mt19937((unsigned int)std::chrono::system_clock::now().time_since_epoch().count() + id)),
		inputFileDist(0, entryNum - 1),
		max_playout_num(playout_num),
		playout(0),
		ply(0),
		search_param_black(0),
		search_param_white(0),
		random_temperature_black(1.4),
		random_temperature_white(1.4),
		previous_kldgain(0.0),
		first_normal_move(true),
		states(MAX_MOVE + 1) {
		pos_root = new Position(DefaultStartPositionSFEN, s.thisptr);
		usi_engine_turn = (grp->usi_engines.size() > 0 && id < usi_engine_num) ? rnd(*mt) % 2 : -1;
		prev_visit.reserve(UCT_CHILD_MAX);
		v_noise.reserve(UCT_CHILD_MAX);
	}
	UCTSearcher(UCTSearcher&& o) : nn_cache(o.nn_cache) {} // not use
	~UCTSearcher() {
		// USIエンジンが思考中の場合待機する
		if (usi_engine_turn >= 0) {
			grp->usi_engines[id % usi_threads].WaitThinking();
		}

		delete pos_root;
	}

	void Playout(visitor_t& visitor);
	void NextStep();

private:
	float UctSearch(Position* pos, child_node_t* parent, uct_node_t* current, visitor_t& visitor);
	int SelectMaxUcbChild(child_node_t* parent, uct_node_t* current);
	bool InterruptionCheck(const int playout_count, const int extension_times);
	void NextPly(const Move move);
	void NextGame();

	// キャッシュからnnrateをコピー
	void CopyNNRate(uct_node_t* node, const vector<float>& nnrate) {
		child_node_t* uct_child = node->child.get();
		for (int i = 0; i < node->child_num; i++) {
			uct_child[i].nnrate = nnrate[i];
		}
	}

	UCTSearcherGroup* grp;
	int id;

	unique_ptr<std::mt19937_64> mt_64;
	unique_ptr<std::mt19937> mt;

	// ルートノード
	std::unique_ptr<uct_node_t> root_node;

	// NNキャッシュ(UCTSearcherGroupで共有)
	NNCache& nn_cache;

	std::string kif;

	int max_playout_num;
	int playout;
	int ply;
	int winrate_count = 0;
	int search_param_black = 0;
	int search_param_white = 0;
	float previous_kldgain = 0.0;
	bool first_normal_move = true;
	float random_temperature_black = 1.4;
	float random_temperature_white = 1.4;
	GameResult gameResult;
	u8 reason;

	std::vector<StateInfo> states;
	uniform_int_distribution<s64> inputFileDist;

	// 局面管理と探索スレッド
	Position* pos_root;

	// ノイズにより選んだ回数
	std::vector<int> prev_visit;
	std::vector<float> v_noise;

	// 詰み探索のステータス
	MateSearchEntry::State mate_status;

	// 開始局面
	HuffmanCodedPos hcp;

	// USIエンジン
	int usi_engine_turn; // -1:未使用、0:偶数手、1:奇数手
	std::string usi_position;

	// 出力棋譜データ
	struct Record {
		Record() {}
		Record(const u16 selectedMove16, const s16 eval) : selectedMove16(selectedMove16), eval(eval) {}

		u16 selectedMove16; // 指し手
		s16 eval; // 評価値
		std::vector<MoveVisits> candidates;
	};
	std::vector<Record> records;

	// 局面追加
	// 訓練に使用しない手はtrainningをfalseにする
	void AddRecord(Move move, s16 eval, bool trainning, bool is_mate = false) {
		Record& record = records.emplace_back(
			static_cast<u16>(move.value()),
			eval
		);
		if (trainning) {
			const auto child = root_node->child.get();
			record.candidates.reserve(root_node->child_num);
			const size_t child_num = root_node->child_num;
			const child_node_t* uct_child = root_node->child.get();
			int max_child = 0, max_child_nonoise = 0;
			const int sum = root_node->move_count;
			const float sqrt_sum = sqrtf((float)sum);
			const float c = FastLog((sum + c_base_root + 1.0f) / c_base_root) + c_init_root;
			const float init_u = sum == 0 ? 1.0f : sqrt_sum;

			// 探索回数が最も多い手と次に多い手を求める
			int max_index = 0;
			int max = 0;
			int max2 = 0;
			float max_value = 0;
			float max_ucb = 0;
			for (int i = 0; i < child_num; i++) {
				const WinType win = uct_child[i].win;
				const int move_count = uct_child[i].move_count;

				float q = (float)(win / move_count);
				float u = sqrt_sum / (1 + move_count);

				float rate = uct_child[i].nnrate;
				const float ucb_value = q + c * u * rate;
				if ((max <= 100 && child[i].move_count > max) || (child[i].move_count > 100 && max_ucb < ucb_value)) {
					max = child[i].move_count;
					max_index = i;
					max_value = child[i].win / child[i].move_count;
					max_ucb = ucb_value;
				}
			}
			int sum_count = 0;
			// UCB値最大の手を求める
			for (int i = 0; i < child_num; i++) {
				const WinType win = uct_child[i].win;
				int move_count = uct_child[i].move_count;
				float rate = uct_child[i].nnrate;
				int prev_move_count = move_count;
				while (max_index != i && move_count > 0) {
					float q = (float)(win / move_count);
					float u = sqrt_sum / (1 + move_count);
					const float ucb_value = q + c * u * rate;
					if (max_value > ucb_value) {
						move_count--;
						sum_playouts_remove++;
					}
					else {
						break;
					}
				}
				//if (rate < 0.002 && move_count >= 50) {
				//	std::cerr << move_count << " " << rate << " " << uct_child[i].noise << " " << rate * 0.75 + uct_child[i].noise * 0.25 << std::endl;
				//}
				//if (prev_move_count - move_count > 5 || (move_count > 1 && prev_move_count/move_count >= 2)) {
				//	std::cerr << max << " " << prev_move_count << " " << move_count << " " << rate << " " << uct_child[i].noise << std::endl;
				//	}
				if (move_count > 0) {
					if (is_mate && child[i].move == move) {
						if (move_count * 1.5 < sum) {
							move_count += sum;
						}
					}
					record.candidates.emplace_back(
						static_cast<u16>(child[i].move.value()),
						static_cast<u16>(move_count)
					);
				}
			}
			idx++;
		}
	}
	// 局面出力
	void WriteRecords(std::ofstream& ofs, HuffmanCodedPosAndEval3& hcpe3) {
		std::unique_lock<Mutex> lock(omutex);
		ofs.write(reinterpret_cast<char*>(&hcpe3), sizeof(HuffmanCodedPosAndEval3));
		for (auto& record : records) {
			MoveInfo moveInfo{ record.selectedMove16, record.eval, static_cast<u16>(record.candidates.size()) };
			ofs.write(reinterpret_cast<char*>(&moveInfo), sizeof(MoveInfo));
			if (record.candidates.size() > 0) {
				ofs.write(reinterpret_cast<char*>(record.candidates.data()), sizeof(MoveVisits) * record.candidates.size());
				madeTeacherNodes++;
			}
		}
	}
};

class UCTSearcherGroupPair {
public:
	UCTSearcherGroupPair(const int gpu_id, const int policy_value_batch_maxsize) : nn(nullptr), gpu_id(gpu_id), policy_value_batch_maxsize(policy_value_batch_maxsize) {
		groups.reserve(threads);
		for (int i = 0; i < threads; i++)
			groups.emplace_back(gpu_id, i, policy_value_batch_maxsize, this);
	}
	UCTSearcherGroupPair(UCTSearcherGroupPair&& o) {} // not use
	~UCTSearcherGroupPair() {
		delete nn;
	}
	void InitGPU() {
		mutex_all_gpu.lock();
		if (nn == nullptr) {
			nn = (NN*)new NNTensorRT(model_path.c_str(), gpu_id, policy_value_batch_maxsize);
		}
		mutex_all_gpu.unlock();
	}
	void nn_forward(const int batch_size, packed_features1_t* x1, packed_features2_t* x2, DType* y1, DType* y2) {
		mutex_gpu.lock();
		nn->forward(batch_size, x1, x2, y1, y2);
		mutex_gpu.unlock();
	}
	int Running() {
		int running = 0;
		for (int i = 0; i < threads; i++)
			running += groups[i].running;
		return running;
	}
	void Run() {
		for (int i = 0; i < threads; i++)
			groups[i].Run();
	}
	void Join() {
		for (int i = 0; i < threads; i++)
			groups[i].Join();
	}

private:
	vector<UCTSearcherGroup> groups;
	int policy_value_batch_maxsize;
	int gpu_id;

	// neural network
	NN* nn;
	// mutex for gpu
	mutex mutex_gpu;
};

void
UCTSearcherGroup::Initialize()
{
	// USIエンジン起動
	if (usi_engine_path != "" && usi_engine_num != 0) {
		std::vector<std::pair<std::string, std::string>> options;
		std::istringstream ss(usi_options);
		std::string field;
		while (std::getline(ss, field, ',')) {
			const auto pos = field.find_first_of(":");
			options.emplace_back(field.substr(0, pos), field.substr(pos + 1));

		}
		usi_engines.reserve(usi_threads);
		for (int i = 0; i < usi_threads; ++i) {
			usi_engines.emplace_back(usi_engine_path, options, (usi_engine_num + usi_threads - 1) / usi_threads);
		}
	}

	// キューを動的に確保する
	checkCudaErrors(cudaHostAlloc((void**)&features1, sizeof(packed_features1_t) * policy_value_batch_maxsize, cudaHostAllocPortable));
	checkCudaErrors(cudaHostAlloc((void**)&features2, sizeof(packed_features2_t) * policy_value_batch_maxsize, cudaHostAllocPortable));
	policy_value_batch = new batch_element_t[policy_value_batch_maxsize];

	// UCTSearcher
	searchers.clear();
	searchers.reserve(policy_value_batch_maxsize);
	for (int i = 0; i < policy_value_batch_maxsize; i++) {
		searchers.emplace_back(this, nn_cache, i, entryNum);
	}

	checkCudaErrors(cudaHostAlloc((void**)&y1, MAX_MOVE_LABEL_NUM * (size_t)SquareNum * policy_value_batch_maxsize * sizeof(DType), cudaHostAllocPortable));
	checkCudaErrors(cudaHostAlloc((void**)&y2, policy_value_batch_maxsize * sizeof(DType), cudaHostAllocPortable));

	// 詰み探索
	if (ROOT_MATE_SEARCH_DEPTH > 0) {
		dfpn.init();
		dfpn.set_max_search_node(MATE_SEARCH_MAX_NODE);
		dfpn.set_maxdepth(ROOT_MATE_SEARCH_DEPTH);
		mate_search_slot = new MateSearchEntry[policy_value_batch_maxsize];
	}
}

// 連続自己対局
void UCTSearcherGroup::SelfPlay()
{
	// スレッドにGPUIDを関連付けてから初期化する
	cudaSetDevice(gpu_id);

	parent->InitGPU();

	// 探索経路のバッチ
	vector<visitor_t> visitor_batch(policy_value_batch_maxsize);

	// 全スレッドが生成した局面数が生成局面数以上になったら終了
	while (madeTeacherNodes < teacherNodes && !stopflg) {
		current_policy_value_batch_index = 0;

		// すべての対局についてシミュレーションを行う
		for (size_t i = 0; i < (size_t)policy_value_batch_maxsize; i++) {
			UCTSearcher& searcher = searchers[i];
			searcher.Playout(visitor_batch[i]);
		}

		// 評価
		EvalNode();

		// バックアップ
		for (auto& visitor : visitor_batch) {
			auto& trajectories = visitor.trajectories;
			float result = 1.0f - visitor.value_win;
			for (int i = (int)trajectories.size() - 1; i >= 0; i--) {
				auto& current_next = trajectories[i];
				uct_node_t* current = current_next.first;
				const unsigned int next_index = current_next.second;
				child_node_t* uct_child = current->child.get();
				UpdateResult(&uct_child[next_index], result, current);
				result = 1.0f - result;
			}
		}

		// 次のシミュレーションへ
		for (UCTSearcher& searcher : searchers) {
			searcher.NextStep();
		}
	}

	running = false;
}

// スレッド開始
void
UCTSearcherGroup::Run()
{
	// 自己対局用スレッド
	running = true;
	handle_selfplay = new thread([this]() { this->SelfPlay(); });

	// 詰み探索用スレッド
	if (ROOT_MATE_SEARCH_DEPTH > 0) {
		handle_mate_search = new thread([this]() { this->MateSearch(); });
	}
}

// スレッド終了待機
void
UCTSearcherGroup::Join()
{
	// 自己対局用スレッド
	handle_selfplay->join();
	delete handle_selfplay;
	// 詰み探索用スレッド
	if (handle_mate_search) {
		handle_mate_search->join();
		delete handle_mate_search;
	}
}

// 詰み探索スレッド
void UCTSearcherGroup::MateSearch()
{
	deque<int> queue;
	while (running) {
		// キューから取り出す
		mate_search_mutex.lock();
		if (mate_search_queue.size() > 0) {
			queue.swap(mate_search_queue);
			mate_search_mutex.unlock();
		}
		else {
			mate_search_mutex.unlock();
			this_thread::yield();
			continue;
		}

		for (int& id : queue) {
			// 盤面のコピー
			Position pos_copy(*mate_search_slot[id].pos);

			// 詰み探索
			const bool mate = dfpn.dfpn(pos_copy);
			//SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} {} mate:{} nodes:{}", gpu_id, group_id, id, pos_copy.toSFEN(), mate, dfpn.searchedNode);
			if (mate) {
				mate_search_slot[id].move = dfpn.dfpn_move(pos_copy);
				mate_search_slot[id].status = MateSearchEntry::WIN;
			}
			else if (pos_copy.inCheck()) {
				// 自玉に王手がかかっている
				const bool mated = dfpn.dfpn_andnode(pos_copy);
				//SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} {} mate_andnode:{} nodes:{}", gpu_id, group_id, id, pos_copy.toSFEN(), mated, dfpn.searchedNode);
				mate_search_slot[id].status = mated ? MateSearchEntry::LOSE : MateSearchEntry::NOMATE;
			}
			else
				mate_search_slot[id].status = MateSearchEntry::NOMATE;
		}
		queue.clear();
	}
}

//////////////////////////////////////////////
//  UCT探索を行う関数                        //
//  1回の呼び出しにつき, 1プレイアウトする    //
//////////////////////////////////////////////
float
UCTSearcher::UctSearch(Position* pos, child_node_t* parent, uct_node_t* current, visitor_t& visitor)
{
	float result;
	child_node_t* uct_child = current->child.get();
	auto& trajectories = visitor.trajectories;

	// 子ノードへのポインタ配列が初期化されていない場合、初期化する
	if (!current->child_nodes) current->InitChildNodes();
	// UCB値最大の手を求める
	const unsigned int next_index = SelectMaxUcbChild(parent, current);
	// 選んだ手を着手
	StateInfo st;
	pos->doMove(uct_child[next_index].move, st);

	// ノードの展開の確認
	if (!current->child_nodes[next_index]) {
		// ノードの作成
		uct_node_t* child_node = current->CreateChildNode(next_index);

		// 経路を記録
		trajectories.emplace_back(current, next_index);

		// 千日手チェック
		int isDraw = 0;
		switch (pos->isDraw(16)) {
		case NotRepetition: break;
		case RepetitionDraw: isDraw = 2; break; // Draw
		case RepetitionWin: isDraw = 1; break;
		case RepetitionLose: isDraw = -1; break;
		case RepetitionSuperior: isDraw = 1; break;
		case RepetitionInferior: isDraw = -1; break;
		default: UNREACHABLE;
		}

		// 千日手の場合、ValueNetの値を使用しない
		if (isDraw != 0) {
			if (isDraw == 1) {
				uct_child[next_index].SetWin();
				result = 0.0f;
			}
			else if (isDraw == -1) {
				uct_child[next_index].SetLose();
				result = 1.0f;
			}
			else {
				uct_child[next_index].SetDraw();
				result = 0.5f;
			}
		}
		else if (nn_cache.ContainsKey(pos->getKey())) {
			NNCacheLock cache_lock(&nn_cache, pos->getKey());
			// キャッシュヒット
			// 候補手を展開する
			child_node->ExpandNode(pos);
			assert(cache_lock->nnrate.size() == child_node->child_num);
			// キャッシュからnnrateをコピー
			CopyNNRate(child_node, cache_lock->nnrate);
			// 経路により詰み探索の結果が異なるためキャッシュヒットしても詰みの場合があるが、速度が落ちるため詰みチェックは行わない
			result = 1.0f - cache_lock->value_win;
		}
		else {
			// 詰みチェック
			int isMate = 0;
			if (!pos->inCheck()) {
				if (mateMoveInOddPly<MATE_SEARCH_DEPTH, false>(*pos)) {
					isMate = 1;
				}
				// 入玉勝ちかどうかを判定
				else if (nyugyoku<false>(*pos)) {
					isMate = 1;
				}
			}
			else {
				if (mateMoveInOddPly<MATE_SEARCH_DEPTH, true>(*pos)) {
					isMate = 1;
				}
				// 偶数手詰めは親のノードの奇数手詰めでチェックされているためチェックしない
				/*else if (mateMoveInEvenPly(*pos, MATE_SEARCH_DEPTH - 1)) {
					isMate = -1;
				}*/
			}


			// 詰みの場合、ValueNetの値を上書き
			if (isMate == 1) {
				uct_child[next_index].SetWin();
				result = 0.0f;
			}
			/*else if (isMate == -1) {
				uct_node[child_index].value_win = VALUE_LOSE;
				// 子ノードに一つでも負けがあれば、自ノードを勝ちにできる
				current->value_win = VALUE_WIN;
				result = 1.0f;
			}*/
			else {
				// 候補手を展開する（千日手や詰みの場合は候補手の展開が不要なため、タイミングを遅らせる）
				child_node->ExpandNode(pos);
				if (child_node->child_num == 0) {
					// 詰み
					uct_child[next_index].SetLose();
					result = 1.0f;
				}
				else
				{
					// ノードをキューに追加
					grp->QueuingNode(pos, child_node, &visitor.value_win);
					return QUEUING;
				}
			}
		}
		child_node->SetEvaled();
	}
	else {
		// 経路を記録
		trajectories.emplace_back(current, next_index);

		uct_node_t* next_node = current->child_nodes[next_index].get();

		if (uct_child[next_index].IsWin()) {
			// 詰み、もしくはRepetitionWinかRepetitionSuperior
			result = 0.0f;  // 反転して値を返すため0を返す
		}
		else if (uct_child[next_index].IsLose()) {
			// 自玉の詰み、もしくはRepetitionLoseかRepetitionInferior
			result = 1.0f; // 反転して値を返すため1を返す
		}
		// 千日手チェック
		else if (uct_child[next_index].IsDraw()) {
			result = 0.5f;
		}
		// 詰みのチェック
		else if (next_node->child_num == 0) {
			result = 1.0f; // 反転して値を返すため1を返す
		}
		else {
			// 手番を入れ替えて1手深く読む
			result = UctSearch(pos, &uct_child[next_index], next_node, visitor);
		}
	}

	if (result == QUEUING)
		return result;

	// 探索結果の反映
	UpdateResult(&uct_child[next_index], result, current);

	return 1.0f - result;
}

/////////////////////////////////////////////////////
//  UCBが最大となる子ノードのインデックスを返す関数  //
/////////////////////////////////////////////////////
int
UCTSearcher::SelectMaxUcbChild(child_node_t* parent, uct_node_t* current)
{
	const child_node_t* uct_child = current->child.get();
	const int child_num = current->child_num;
	int max_child = 0, max_child_nonoise = 0;
	const int sum = current->move_count;
	const WinType sum_win = current->win;
	float q, u, max_value, max_value_nonoise;
	int child_win_count = 0;

	max_value = max_value_nonoise = -FLT_MAX;

	const float sqrt_sum = sqrtf((float)sum);
	float c = parent == nullptr ?
		FastLog((sum + c_base_root + 1.0f) / c_base_root) + c_init_root :
		FastLog((sum + c_base + 1.0f) / c_base) + (c_init + search_param_diff[ply % 2 == 1 ? search_param_black : search_param_white]);
	const float fpu_reduction = (parent == nullptr ? 0.0f : c_fpu_reduction) * sqrtf(current->visited_nnrate);
	const float parent_q = sum_win > 0 ? std::max(0.0f, (float)(sum_win / sum) - fpu_reduction) : 0.0f;
	const float init_u = sum == 0 ? 1.0f : sqrt_sum;

	// UCB値最大の手を求める
	// float c_dynamic = c;
	//if (sum > 10) {
	//	double q = sum_win / sum;
	//	double win2 = current->win2;
	//	float v = sqrt((win2 / sum) - q * q);
	//	c_dynamic = c * v * DYNAMIC_PARAM;
	//}
	int child_num_valid = 0;
	for (int i = 0; i < child_num; i++) {
		//if (uct_child[i].IsWin()) {
		//	child_win_count++;
		//	// 負けが確定しているノードは選択しない
		//	continue;
		//}
		//else if (uct_child[i].IsLose()) {
		//	// 子ノードに一つでも負けがあれば、自ノードを勝ちにできる
		//	if (parent != nullptr)
		//		parent->SetWin();
		//	// 勝ちが確定しているため、選択する
		//	return i;
		//}

		const WinType win = uct_child[i].win;
		const WinType win2 = uct_child[i].win2;
		const int move_count = uct_child[i].move_count;
		float c_dynamic = c;
		if (move_count == 0) {
			// 未探索のノードの価値に、親ノードの価値を使用する
			q = parent_q;
			u = init_u;
		}
		else {
			q = (float)(win / move_count);
			u = sqrt_sum / (1 + move_count);
			if (move_count >= 3) {
				const float v = sqrt((move_count / (move_count - 1)) * max(1e-5f, (win2 / move_count - q * q)));
				const float r = 1.0 / sqrtf(move_count) + 0.25;
				c_dynamic = r * c + (1.0 - r) * (c * v * DYNAMIC_PARAM);
			}
		}

		float rate = uct_child[i].nnrate;
		float noise_rate = parent == nullptr ? uct_child[i].nnrate * ALPHA_D + (1.0f - ALPHA_D) * uct_child[i].noise : uct_child[i].nnrate;
		const float ucb_value = q + c_dynamic * u * noise_rate;

		if (ucb_value > max_value) {
			max_value = ucb_value;
			max_child = i;
		}
	}
	if (child_win_count == child_num) {
		// 子ノードがすべて勝ちのため、自ノードを負けにする
		if (parent != nullptr)
			parent->SetLose();
	}
	else {
		// for FPU reduction
		current->visited_nnrate += uct_child[max_child].nnrate;
	}

	return max_child;
}

//////////////////////////////////////
//  ノードをキューに追加            //
//////////////////////////////////////
void
UCTSearcherGroup::QueuingNode(const Position* pos, uct_node_t* node, float* value_win)
{
	// set all zero
	std::fill_n(features1[current_policy_value_batch_index], sizeof(packed_features1_t), 0);
	std::fill_n(features2[current_policy_value_batch_index], sizeof(packed_features2_t), 0);

	make_input_features(*pos, features1[current_policy_value_batch_index], features2[current_policy_value_batch_index]);
	policy_value_batch[current_policy_value_batch_index] = { node, pos->turn(), pos->getKey(), value_win };
	current_policy_value_batch_index++;
}
//////////////////////////
//  探索打ち止めの確認  //
//////////////////////////
bool
UCTSearcher::InterruptionCheck(const int playout_count, const int extension_times)
{
	int max_index = 0;
	int max = 0, second = 0;
	const int child_num = root_node->child_num;
	const int rest = max_playout_num - playout_count;
	const child_node_t* uct_child = root_node->child.get();

	if (playout_count % INTERVAL != 0 || playout_count == 0)
		return false;

	if (extension_times == 0 && playout_count >= max_playout_num / 4) {
		return true;
	}

	if (playout_count >= max_playout_num) {
		std::cout << "Interruption" << " " << playout_count << std::endl;
		return true;
	}

	float kldgain = 0.0;
	if (playout_count == INTERVAL) {
		for (int i = 0; i < child_num; i++) {
			prev_visit[i] = uct_child[i].move_count;
		}
		return false;
	}
	float sum1 = playout_count - INTERVAL;
	float sum2 = playout_count;
	for (int i = 0; i < child_num; i++) {
		float old_p = prev_visit[i] / sum1;
		float new_p = uct_child[i].move_count / sum2;

		if (old_p != 0 && new_p != 0) {
			kldgain += old_p * log(old_p / new_p);
		}
	}
	kldgain /= INTERVAL;
	if (previous_kldgain > 0 && (previous_kldgain + kldgain) * 0.5 < KLDGAIN_THRESHOLD) {
		return true;
	}
	for (int i = 0; i < child_num; i++) {
		prev_visit[i] = uct_child[i].move_count;
	}
	previous_kldgain = kldgain;
	return false;
}
void computeDirichletAlphaDistribution(int child_num, std::vector<double>& x, std::vector<float>& alphaDistr) {
	//Half of the alpha weight are uniform.
	//The other half are shaped based on the log of the existing policy.
	double logPolicySum = 0.0;
	for (int i = 0; i < child_num; i++) {
		if (x[i] >= 0) {
			alphaDistr[i] = log(std::min(0.01, (double)x[i]) + 1e-20);
			logPolicySum += alphaDistr[i];
		}
	}
	double logPolicyMean = logPolicySum / child_num;
	double alphaPropSum = 0.0;
	for (int i = 0; i < child_num; i++) {
		if (x[i] >= 0) {
			alphaDistr[i] = std::max(0.0, alphaDistr[i] - logPolicyMean);
			alphaPropSum += alphaDistr[i];
		}
	}
	double uniformProb = 1.0 / child_num;
	if (alphaPropSum <= 0.0) {
		for (int i = 0; i < child_num; i++) {
			if (x[i] >= 0)
				alphaDistr[i] = uniformProb;
		}
	}
	else {
		for (int i = 0; i < child_num; i++) {
			if (x[i] >= 0)
				alphaDistr[i] = 0.5 * (alphaDistr[i] / alphaPropSum + uniformProb);
		}
	}
}

// 局面の評価
void UCTSearcherGroup::EvalNode() {
	if (current_policy_value_batch_index == 0)
		return;

	const int policy_value_batch_size = current_policy_value_batch_index;

	// predict
	parent->nn_forward(policy_value_batch_size, features1, features2, y1, y2);

	DType(*logits)[MAX_MOVE_LABEL_NUM * SquareNum] = reinterpret_cast<DType(*)[MAX_MOVE_LABEL_NUM * SquareNum]>(y1);
	DType* value = y2;

	for (int i = 0; i < policy_value_batch_size; i++, logits++, value++) {
		uct_node_t* node = policy_value_batch[i].node;
		const Color color = policy_value_batch[i].color;

		const int child_num = node->child_num;
		child_node_t* uct_child = node->child.get();
		std::vector<double> root_x(child_num);
		// 合法手一覧
		for (int j = 0; j < child_num; j++) {
			Move move = uct_child[j].move;
			const int move_label = make_move_label((u16)move.proFromAndTo(), color);
			const float logit = (float)(*logits)[move_label];
			uct_child[j].nnrate = logit;
			root_x[j] = logit;
		}

		// Boltzmann distribution
		if (policy_value_batch[i].value_win)
			softmax_temperature_with_normalize(uct_child, child_num);
		else {
			std::vector<float> r(child_num);
			softmax_temperature_with_normalize_dirichlet(root_x, child_num);
			
			computeDirichletAlphaDistribution(child_num, root_x, r);
			random_dirichlet(mt_64_global, r, 10.0f);
			for (int j = 0; j < child_num; j++) {
				uct_child[j].noise = r[j];
			}
			softmax_temperature_with_normalize_root(uct_child, child_num);
		}
		auto req = make_unique<CachedNNRequest>(child_num);
		for (int j = 0; j < child_num; j++) {
			req->nnrate[j] = uct_child[j].nnrate;
		}

		const float value_win = (float)*value;

		req->value_win = value_win;
		nn_cache.Insert(policy_value_batch[i].key, std::move(req));

		if (policy_value_batch[i].value_win)
			*policy_value_batch[i].value_win = value_win;

		node->SetEvaled();
	}
}

// シミュレーションを1回行う
void UCTSearcher::Playout(visitor_t& visitor)
{
	while (true) {
		visitor.trajectories.clear();
		// 手番開始
		if (playout == 0) {
			// 新しいゲーム開始
			if (ply == 0) {
				ply = 1;
				first_normal_move = true;
				random_temperature_black = RANDOM_TEMPERATURE;
				random_temperature_white = RANDOM_TEMPERATURE * 0.98f;
				//search_param_black = (*mt_64)() % 3;
				//search_param_white = (*mt_64)() % 3;
				//while (search_param_black == search_param_white) {
				//	search_param_white = (*mt_64)() % 3;
				//}
				search_param_black = 1;
				search_param_white = 1;
				//// 開始局面を局面集からランダムに選ぶ
				//{
				//	std::unique_lock<Mutex> lock(imutex);
				//	ifs.seekg(inputFileDist(*mt_64) * sizeof(HuffmanCodedPos), std::ios_base::beg);
				//	ifs.read(reinterpret_cast<char*>(&hcp), sizeof(hcp));
				//}
				// pos_root->set(DefaultStartPositionSFEN);
				// hcp = pos_root->toHuffmanCodedPos();

				// 開始局面を局面集からランダムに選ぶ
				{
					std::unique_lock<Mutex> lock(imutex);
					ifs.seekg(inputFileDist(*mt_64) * sizeof(HuffmanCodedPos), std::ios_base::beg);
					ifs.read(reinterpret_cast<char*>(&hcp), sizeof(hcp));
				}
				setPosition(*pos_root, hcp);
				kif.clear();
				kif += "position ";
				kif += pos_root->toSFEN() + " moves ";

				// SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {}", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN());

				records.clear();
				reason = 0;
				root_node.reset();

				// USIエンジン
				if (usi_engine_turn >= 0) {
					// 開始局面設定
					usi_position = "position " + pos_root->toSFEN() + " moves";

					// 先手後手指定
					if (usi_turn == Black)
						usi_engine_turn = pos_root->turn() == Black ? 1 : 0;
					else if (usi_turn == White)
						usi_engine_turn = pos_root->turn() == White ? 1 : 0;
					else
						usi_engine_turn = rnd(*mt) % 2;

					if (usi_engine_turn == 1 && RANDOM_MOVE == 0) {
						grp->usi_engines[id % usi_threads].ThinkAsync(id / usi_threads, *pos_root, usi_position, int(usi_byoyomi / 1000));
						return;
					}
				}
			}
			else if (ply % 2 == usi_engine_turn && (ply > RANDOM_MOVE)) {
				return;
			}

			if (!root_node || !REUSE_SUBTREE) {
				// ルートノード作成(以前のノードは再利用しないで破棄する)
				root_node = std::make_unique<uct_node_t>();

				// ルートノード展開
				root_node->ExpandNode(pos_root);
			}
			prev_visit.resize(root_node->child_num);
			v_noise.resize(root_node->child_num);
			std::fill(prev_visit.begin(), prev_visit.end(), 0);
			// 詰みのチェック
			if (root_node->child_num == 0) {
				gameResult = (pos_root->turn() == Black) ? GameResult::WhiteWin : GameResult::BlackWin;
				NextGame();
				continue;
			}
			else if (root_node->child_num == 1) {
				// 1手しかないときは、その手を指して次の手番へ
				const Move move = root_node->child[0].move;
				// SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} skip:{}", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN(), move.toUSI());
				AddRecord(move, 0, false);
				NextPly(move);
				continue;
			}
			else if (nyugyoku(*pos_root)) {
				// 入玉宣言勝ち
				gameResult = (pos_root->turn() == Black) ? GameResult::BlackWin : GameResult::WhiteWin;
				reason = GAMERESULT_NYUGYOKU;
				if (records.size() > 0)
					++nyugyokus;
				NextGame();
				continue;
			}

			// ルート局面を詰み探索キューに追加
			if (ROOT_MATE_SEARCH_DEPTH > 0) {
				mate_status = MateSearchEntry::RUNING;
				grp->QueuingMateSearch(pos_root, id);
			}
			
			//// ルート局面をキューに追加
			//if (!root_node->IsEvaled()) {
			//	NNCacheLock cache_lock(&nn_cache, pos_root->getKey());
			//	if (!cache_lock || cache_lock->nnrate.size() == 0 || true) {
			//		grp->QueuingNode(pos_root, root_node.get(), nullptr);
			//		return;
			//	}
			//	else {
			//		assert(cache_lock->nnrate.size() == root_node->child_num);
			//		// キャッシュからnnrateをコピー
			//		CopyNNRate(root_node.get(), cache_lock->nnrate);
			//	}
			//}
			if (!root_node->IsEvaled()) {
				NNCacheLock cache_lock(&nn_cache, pos_root->getKey());
				grp->QueuingNode(pos_root, root_node.get(), nullptr);
				return;
			}
		}
		// 盤面のコピー
		Position pos_copy(*pos_root);
		// プレイアウト
		const float result = UctSearch(&pos_copy, nullptr, root_node.get(), visitor);
		if (result != QUEUING) {
			NextStep();
			continue;
		}

		return;
	}
}

// 次の手に進める
void UCTSearcher::NextStep()
{
	// USIエンジン
	if (ply % 2 == usi_engine_turn && (ply > RANDOM_MOVE)) {
		const auto& result = grp->usi_engines[id % usi_threads].ThinkDone(id / usi_threads);
		if (result.move == Move::moveNone())
			return;

		if (result.move == moveResign()) {
			gameResult = (pos_root->turn() == Black) ? GameResult::WhiteWin : GameResult::BlackWin;
			NextGame();
			return;
		}
		else if (result.move == moveWin()) {
			gameResult = (pos_root->turn() == Black) ? GameResult::BlackWin : GameResult::WhiteWin;
			reason = GAMERESULT_NYUGYOKU;
			NextGame();
			return;
		}
		else if (result.move == moveAbort()) {
			if (stopflg)
				return;
			throw std::runtime_error("usi engine abort");
		}
		//SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} usi_move:{} usi_score:{}", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN(), result.move.toUSI(), result.score);

		AddRecord(result.move, result.score, false);
		NextPly(result.move);
		return;
	}

	//// 詰み探索の結果を調べる
	//if (ROOT_MATE_SEARCH_DEPTH > 0 && mate_status == MateSearchEntry::RUNING) {
	//	mate_status = grp->GetMateSearchStatus(id);
	//	if (mate_status != MateSearchEntry::RUNING) {
	//		// 詰みの場合
	//		switch (mate_status) {
	//		case MateSearchEntry::WIN:
	//		{
	//			SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} mate win", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN());
	//			gameResult = (pos_root->turn() == Black) ? BlackWin : WhiteWin;

	//			// 局面追加（ランダム局面は除く）
	//			if ((ply > RANDOM_MOVE))
	//				AddRecord(grp->GetMateSearchMove(id), 10000, true);

	//			NextGame();
	//			return;
	//		}
	//		case MateSearchEntry::LOSE:
	//			SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} mate lose", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN());
	//			gameResult = (pos_root->turn() == Black) ? WhiteWin : BlackWin;
	//			NextGame();
	//			return;
	//		}
	//	}
	//}

	// プレイアウト回数加算
	playout++;

	// 探索終了判定
	if (InterruptionCheck(playout, ((ply > RANDOM_MOVE)) ? EXTENSION_TIMES : 0)) {
		// 平均プレイアウト数を計測
		sum_playouts += playout;
		double v = sqrt(max(1e-8, (root_node->win2 / double(playout)) - (root_node->win / double(playout)) * (root_node->win / double(playout))));
		double c_tmp = FastLog((playout + c_base_root + 1.0f) / c_base_root) + c_init_root;
		sum_c_dynamic += c_tmp * v * DYNAMIC_PARAM;
		
		++sum_nodes;
		//std::cerr << sum_c_dynamic / sum_nodes << " " << c_tmp * v * 6.5 << std::endl;
		// 詰み探索の結果を待つ
		mate_status = grp->GetMateSearchStatus(id);
		if (ROOT_MATE_SEARCH_DEPTH > 0 && mate_status != MateSearchEntry::RUNING) {
			// 詰みの場合
			switch (mate_status) {
			case MateSearchEntry::WIN:
				SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} mate win", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN());
				//gameResult = (pos_root->turn() == Black) ? BlackWin : WhiteWin;

				// 局面追加（初期局面は除く）
				if ((ply > RANDOM_MOVE))
					AddRecord(grp->GetMateSearchMove(id), 10000, true, true);
				winrate_count += 1;
				NextPly(grp->GetMateSearchMove(id));
				return;
				//NextGame();
				//return;
			//case MateSearchEntry::LOSE:
				//SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} mate lose", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN());
				//gameResult = (pos_root->turn() == Black) ? WhiteWin : BlackWin;
				//NextGame();
				//return;
			}
		}

		const child_node_t* uct_child = root_node->child.get();
		float best_wp;
		Move best_move;
		if (!((ply > RANDOM_MOVE))) {
			// N手までは訪問数に応じた確率で選択する
			const auto child_num = root_node->child_num;

			// 訪問回数順にソート
			std::vector<const child_node_t*> sorted_uct_childs;
			sorted_uct_childs.reserve(child_num);
			for (int i = 0; i < child_num; i++)
				sorted_uct_childs.emplace_back(&uct_child[i]);
			std::stable_sort(sorted_uct_childs.begin(), sorted_uct_childs.end(), compare_child_node_ptr_descending);

			// 訪問数が最大のノードの価値の一定割合以下は除外
			const auto max_move_count_child = sorted_uct_childs[0];
			const int step = (ply - 1) / 2;
			const auto best_wp_ = max_move_count_child->win / max_move_count_child->move_count;
			vector<double> probabilities;
			probabilities.reserve(child_num);
			float temp_c = 1.0;
			float balance = -0.03;
			float lower_limit = 0.410 + (pos_root->turn() == White ? balance : -balance);
			float upper_limit = 0.590 + (pos_root->turn() == White ? balance : -balance);
			if (best_wp_ < lower_limit) {
				temp_c = max(0.2, 0.75 - (lower_limit - best_wp_) * 5.0);
			}
			if (best_wp_ > upper_limit) {
				temp_c = min(2.0, 1.35 + (best_wp_ - upper_limit) * 5.0);
			}
			//float r = 25;
			//const float temperature = ((RANDOM_TEMPERATURE * 2) / (1.0 + exp(ply / r))) * temp_c;
			const float temperature = (pos_root->turn() == Black) ? (random_temperature_black) : random_temperature_white;
			const float reciprocal_temperature = 1.0f / temperature;

			for (int i = 0; i < std::min<int>(12, child_num); i++) {
				if (sorted_uct_childs[i]->move_count == 0) break;
				const auto win = sorted_uct_childs[i]->win / sorted_uct_childs[i]->move_count;
				if (win < best_wp_ - 0.060 + min(0.020, ply * 0.0015)) continue;
				if (win < lower_limit && win < best_wp_ - 0.015) continue;
				int move_count = sorted_uct_childs[i]->move_count + sorted_uct_childs[i]->nnrate * 4;
				float correct_num = win >= lower_limit ? 7.5 : 15.5;
				//float move_count_correction = move_count > 20 ? move_count - 10.5 : 1.25 * log(1 + exp(0.8 * (move_count - 10.5)));
				float move_count_correction = min<float>(move_count, (move_count - correct_num) > 20 ? move_count : (4.0 * FastLog(1 + exp(0.70 * (move_count - correct_num)))));
				const auto probability = std::pow(move_count_correction, reciprocal_temperature);
				//if (move_count > 10 && id == 0)
				//	std::cout << id << " " << ply << " " << move_count << " " << move_count_correction << " " << probability << " " << temperature << std::endl;
				probabilities.emplace_back(probability);
				SPDLOG_TRACE(logger, "gpu_id:{} group_id:{} id:{} {}:{} move_count:{} nnrate:{} win_rate:{} probability:{}",
					grp->gpu_id, grp->group_id, id, i, sorted_uct_childs[i]->move.toUSI(), sorted_uct_childs[i]->move_count,
					sorted_uct_childs[i]->nnrate, sorted_uct_childs[i]->win / sorted_uct_childs[i]->move_count, probability);
			}

			discrete_distribution<unsigned int> dist(probabilities.begin(), probabilities.end());
			const auto sorted_select_index = dist(*mt_64);
			best_move = sorted_uct_childs[sorted_select_index]->move;
			best_wp = sorted_uct_childs[sorted_select_index]->win / sorted_uct_childs[sorted_select_index]->move_count;
			int sum_move_count = root_node->move_count;
			float rate = float(sorted_uct_childs[sorted_select_index]->move_count) / sum_move_count;
			const float temperature_drop = RANDOM_TEMPERATURE_DROP * -FastLog(min(1.0, rate * 1.6));
			if (pos_root->turn() == Black) {
				random_temperature_black = max(RANDOM_TEMPERATURE_FINAL, random_temperature_black - temperature_drop);
			}
			else {
				random_temperature_white = max(RANDOM_TEMPERATURE_FINAL, random_temperature_white - temperature_drop);
			}
			if (grp->group_id == 0 && id < 8) {
				SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} random_move:{} winrate:{:.3f} temperature:{:.3f} rate:{:.4f} drop:{:.4f}", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN(), best_move.toUSI(), best_wp, temperature, rate, temperature_drop);
				//SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} random_move:{} winrate:{:.3f} temperature:{:.3f} drop:{:.4f}", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN(), best_move.toUSI(), best_wp, temperature, temperature_drop);
			}
			// 局面追加
			if (TRAIN_RANDOM)
				AddRecord(best_move, value_to_score(best_wp), true);
			else
				AddRecord(best_move, 0, false);

			if ((pos_root->turn() == Black && (best_wp < 0.27 || 0.79 < best_wp)) || (pos_root->turn() == White && (best_wp < 0.21 || 0.73 < best_wp))) {
				SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} winrate:{} Interruption Game",
					grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN(), best_wp);
				NextGame();
				return;
			}
		}
		else {
			// 訪問数に応じた確率で選択する
			const auto child_num = root_node->child_num;

			// 訪問回数順にソート
			std::vector<const child_node_t*> sorted_uct_childs;
			sorted_uct_childs.reserve(child_num);
			for (int i = 0; i < child_num; i++)
				sorted_uct_childs.emplace_back(&uct_child[i]);
			std::stable_sort(sorted_uct_childs.begin(), sorted_uct_childs.end(), compare_child_node_ptr_descending);

			// 訪問数が最大のノードの価値の一定割合以下は除外
			const auto max_move_count_child = sorted_uct_childs[0];
			vector<double> probabilities;
			probabilities.reserve(child_num);
			const float temperature = RANDOM_TEMPERATURE_FINAL;
			const float reciprocal_temperature = 1.0f / temperature;
			for (int i = 0; i < std::min<int>(4, child_num); i++) {
				if (sorted_uct_childs[i]->move_count == 0) break;

				const auto win = sorted_uct_childs[i]->win / sorted_uct_childs[i]->move_count;
				//if (win < cutoff_threshold) break;
				int move_count = sorted_uct_childs[i]->move_count + sorted_uct_childs[i]->nnrate * 4;
				float move_count_correction = min<float>(move_count, (move_count - 8.0) > 20 ? move_count : (4.0 * FastLog(1 + exp(0.70 * (move_count - 8.0)))));
				const auto probability = std::pow(move_count_correction, reciprocal_temperature);
				probabilities.emplace_back(probability);
				SPDLOG_TRACE(logger, "gpu_id:{} group_id:{} id:{} {}:{} move_count:{} nnrate:{} win_rate:{} probability:{}",
					grp->gpu_id, grp->group_id, id, i, sorted_uct_childs[i]->move.toUSI(), sorted_uct_childs[i]->move_count,
					sorted_uct_childs[i]->nnrate, sorted_uct_childs[i]->win / sorted_uct_childs[i]->move_count, probability);
			}
			discrete_distribution<unsigned int> dist(probabilities.begin(), probabilities.end());
			const auto sorted_select_index = dist(*mt_64);
			// 選択した着手の勝率の算出
			best_move = sorted_uct_childs[sorted_select_index]->move;
			best_wp = sorted_uct_childs[sorted_select_index]->win / sorted_uct_childs[sorted_select_index]->move_count;
			if (id <= 0)
				SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} bestmove:{} winrate:{}", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN(), best_move.toUSI(), best_wp);
			if (first_normal_move) {
				first_normal_move = false;
				static int count_distribution[7];
				if (pos_root->turn() == White) {
					best_wp = 1.0 - best_wp;
				}
				if (best_wp <= 0.304) count_distribution[0]++;
				if (0.304 < best_wp && best_wp <= 0.378) count_distribution[1]++; // -625 ~ -375
				if (0.378 < best_wp && best_wp <= 0.459) count_distribution[2]++; // -375 ~ -125
				if (0.459 < best_wp && best_wp <= 0.541) count_distribution[3]++; // -125 ~ 125
				if (0.541 < best_wp && best_wp <= 0.622) count_distribution[4]++;
				if (0.622 < best_wp && best_wp <= 0.696) count_distribution[5]++;
				if (0.696 < best_wp) count_distribution[6]++;
				int a[7];
				for (int c = 0; c < 7; c++) a[c] = count_distribution[c];
				if (grp->group_id == 0)
					SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} winrate:{}: {} {} {} {} {} {} {}",
						grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN(), best_wp, a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
				// randomプレイ終了後の評価値が微妙の場合はすぐ終了する。
				if ((pos_root->turn() == Black && (best_wp < 0.24 || 0.71 < best_wp)) || (pos_root->turn() == White && (best_wp < 0.29 || 0.76 < best_wp))) {
					SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} winrate:{} Interruption Game",
						grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN(), best_wp);
					NextGame();
					return;
				}
				if (pos_root->turn() == White) {
					best_wp = 1.0 - best_wp;
				}
			}

			{
				// 勝率が閾値を超えた場合、ゲーム終了
				const float winrate = abs(best_wp - 0.5f) + 0.5;
				if (ply <= 150 && WINRATE_THRESHOLD <= winrate || 0.998 <= winrate) {
					winrate_count += 1;
					if (winrate_count >= WINRATE_COUNT && best_wp < 0.5) {
						if (pos_root->turn() == Black)
							gameResult = WhiteWin;
						else
							gameResult = BlackWin;

						NextGame();
						return;
					}
					if (usi_engine_turn >= 0 && winrate_count >= WINRATE_COUNT && best_wp > 0.5) {
						if (pos_root->turn() == Black)
							gameResult = BlackWin;
						else
							gameResult = WhiteWin;
						NextGame();
						return;
					}
				}
				else {
					winrate_count = 0;
				}
			}

			// 局面追加
			bool unduplication = true;
			auto itr = st[ply].find(pos_root->getKey());
			if (itr != st[ply].end()) {
				unduplication = false;
			}
			AddRecord(best_move, value_to_score(best_wp), unduplication);
		}

		NextPly(best_move);
	}
}

void UCTSearcher::NextPly(const Move move)
{
	// 一定の手数以上で引き分け
	if (ply >= MAX_MOVE) {
		gameResult = Draw;
		reason = GAMERESULT_MAXMOVE;
		// 最大手数に達した対局は出力しない
		if (!OUT_MAX_MOVE)
			records.clear();
		NextGame();
		return;
	}

	// 着手
	pos_root->doMove(move, states[ply]);
	kif += move.toUSI() + " ";
	ply++;
	if (ply <= 80 && ply % 5 == 0) {
		if (ply >= 20) {
			auto itr = st[ply].find(pos_root->getKey());
			if (itr != st[ply].end()) {
				SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} black_temp:{:.3f} white_temp:{:3f} duplication", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN(), random_temperature_black, random_temperature_white);
				std::cout << kif << std::endl;
			}
		}
		st[ply].insert(pos_root->getKey());
		st_count[ply] += 1;
		if (st_count[ply] % 100 == 0 && ply % 5 == 0)
			std::cout << ply << " " << st[ply].size() << "/" << st_count[ply] << std::endl;
	}

	// 千日手の場合
	switch (pos_root->isDraw(16)) {
	case RepetitionDraw:
		SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} RepetitionDraw", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN());
		gameResult = Draw;
		reason = GAMERESULT_SENNICHITE;
		NextGame();
		return;
	case RepetitionWin:
		SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} RepetitionWin", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN());
		gameResult = (pos_root->turn() == Black) ? BlackWin : WhiteWin;
		NextGame();
		return;
	case RepetitionLose:
		SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} {} RepetitionLose", grp->gpu_id, grp->group_id, id, ply, pos_root->toSFEN());
		gameResult = (pos_root->turn() == Black) ? WhiteWin : BlackWin;
		NextGame();
		return;
	}

	// 次の手番
	max_playout_num = playout_num;
	playout = 0;
	previous_kldgain = 0.0;

	if (usi_engine_turn >= 0) {
		usi_position += " " + move.toUSI();
		if (ply % 2 == usi_engine_turn)
			grp->usi_engines[id % usi_threads].ThinkAsync(id / usi_threads, *pos_root, usi_position, int(usi_byoyomi / 1000));
	}

	// ノード再利用
	if (root_node && REUSE_SUBTREE) {
		bool found = false;
		if (root_node->child_nodes) {
			for (int i = 0; i < root_node->child_num; i++) {
				if (root_node->child[i].move == move && root_node->child_nodes[i] && root_node->child_nodes[i]->child) {
					found = true;
					// 子ノードをルートノードにする
					auto root_node_tmp = std::move(root_node->child_nodes[i]);
					root_node = std::move(root_node_tmp);
					// ルートの訪問回数をクリア
					root_node->move_count = 0;
					root_node->win = 0;
					root_node->visited_nnrate = 0;
					for (int j = 0; j < root_node->child_num; j++) {
						root_node->child[j].move_count = 0;
						root_node->child[j].win = 0;
					}
					break;
				}
			}
		}
		// USIエンジンが選んだ手が見つからない可能性があるため、見つからなかったらルートノードを再作成する
		if (!found) {
			root_node.release();
		}
	}
}

void UCTSearcher::NextGame()
{
	if (gameResult == BlackWin) {
		++black_wins;
		search_param_black_wins[search_param_black][search_param_white]++;
	}
	if (gameResult == WhiteWin) {
		++white_wins;
		search_param_white_wins[search_param_black][search_param_white]++;
	}
	SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} gameResult:{} black_wins:{} white_wins:{}", grp->gpu_id, grp->group_id, id, ply, gameResult, black_wins, white_wins);
	SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} gameResult:{} black_param:{}, white_param:{}, black_wins:{} white_wins:{}", grp->gpu_id, grp->group_id, id, ply, gameResult, search_param_black, search_param_white, search_param_black_wins[search_param_black][search_param_white], search_param_white_wins[search_param_black][search_param_white]);

	// 局面出力
	if (ply >= MIN_MOVE && records.size() > 0) {
		const Color start_turn = (ply % 2 == 1 && pos_root->turn() == Black || ply % 2 == 0 && pos_root->turn() == White) ? Black : White;
		const u8 opponent = usi_engine_turn < 0 ? 0 : (usi_engine_turn == 1 && start_turn == Black || usi_engine_turn == 0 && start_turn == White) ? 1 : 2;
		HuffmanCodedPosAndEval3 hcpe3{
			hcp,
			static_cast<u16>(records.size()),
			static_cast<u8>(gameResult | reason),
			opponent
		};
		WriteRecords((!SPLIT_OPPONENT || opponent == 0) ? ofs : ofs_opponent, hcpe3);
		++games;

		if (gameResult == Draw) {
			++draws;
		}
	}
	std::cout << ply << " " << kif << std::endl;


	// USIエンジンとの対局結果
	if (ply >= MIN_MOVE && usi_engine_turn >= 0) {
		++usi_games;
		if (ply % 2 == 1 && (pos_root->turn() == Black && gameResult == (BlackWin + usi_engine_turn) || pos_root->turn() == White && gameResult == (WhiteWin - usi_engine_turn)) ||
			ply % 2 == 0 && (pos_root->turn() == Black && gameResult == (WhiteWin - usi_engine_turn) || pos_root->turn() == White && gameResult == (BlackWin + usi_engine_turn))) {
			SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} usi_byoyomi:{} usi win", grp->gpu_id, grp->group_id, id, ply, usi_byoyomi / 1000);
			++usi_wins;
			usi_byoyomi += 120;
		}
		else if (gameResult == Draw) {
			SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} usi_byoyomi:{} usi draw", grp->gpu_id, grp->group_id, id, ply, usi_byoyomi / 1000);
			++usi_draws;
		}
		else {
			SPDLOG_DEBUG(logger, "gpu_id:{} group_id:{} id:{} ply:{} usi_byoyomi:{} usi lose", grp->gpu_id, grp->group_id, id, ply, usi_byoyomi / 1000);
			usi_byoyomi -= 120;
		}
	}

	// すぐに終局した初期局面を出力する
	if (OUT_MIN_HCP && ply < MIN_MOVE) {
		std::unique_lock<Mutex> lock(omutex);
		ofs_minhcp.write(reinterpret_cast<char*>(&hcp), sizeof(HuffmanCodedPos));
	}

	// 新しいゲーム
	playout = 0;
	ply = 0;
}

// 教師局面生成
void make_teacher(const char* recordFileName, const char* outputFileName, const vector<int>& gpu_id, const vector<int>& batchsize)
{
	s.init();

	// 初期局面集
	ifs.open(recordFileName, ifstream::in | ifstream::binary | ios::ate);
	if (!ifs) {
		cerr << "Error: cannot open " << recordFileName << endl;
		exit(EXIT_FAILURE);
	}
	entryNum = ifs.tellg() / sizeof(HuffmanCodedPos);

	// 教師局面を保存するファイル
	ofs.open(outputFileName, ios::binary);
	if (!ofs) {
		cerr << "Error: cannot open " << outputFileName << endl;
		exit(EXIT_FAILURE);
	}
	// USIエンジンの教師局面を別ファイルに出力
	if (SPLIT_OPPONENT) {
		std::string filepath{ outputFileName };
		if (filepath.size() >= 6 && filepath.substr(filepath.size() - 6) == ".hcpe3")
			filepath = filepath.substr(0, filepath.size() - 6) + "_opp.hcpe3";
		else
			filepath += "_opp";
		ofs_opponent.open(filepath, ios::binary);
	}
	// 削除候補の初期局面を出力するファイル
	if (OUT_MIN_HCP) ofs_minhcp.open(string(outputFileName) + "_min.hcp", ios::binary);

	vector<UCTSearcherGroupPair> group_pairs;
	group_pairs.reserve(gpu_id.size());
	for (size_t i = 0; i < gpu_id.size(); i++)
		group_pairs.emplace_back(gpu_id[i], batchsize[i]);

	// 探索スレッド開始
	for (size_t i = 0; i < group_pairs.size(); i++)
		group_pairs[i].Run();

	// 進捗状況表示
	auto progressFunc = [&gpu_id, &group_pairs](Timer& t) {
		ostringstream ss;
		for (size_t i = 0; i < gpu_id.size(); i++) {
			if (i > 0) ss << " ";
			ss << gpu_id[i];
		}
		while (!stopflg) {
			std::this_thread::sleep_for(std::chrono::seconds(10)); // 指定秒だけ待機し、進捗を表示する。
			const double progress = static_cast<double>(madeTeacherNodes) / teacherNodes;
			auto elapsed_msec = t.elapsed();
			if (progress > 0.0) // 0 除算を回避する。
				logger->info("Progress:{:.2f}%, nodes:{}, nodes/sec:{:.2f}, games:{}, draw:{}, nyugyoku:{}, ply/game:{:.2f}, playouts/node:{:.2f},  playouts_remove/node:{:.2f} c:{:.4f} gpu id:{}, usi_games:{}, usi_win:{}, usi_draw:{}, Elapsed:{}[s], Remaining:{}[s]",
					std::min(100.0, progress * 100.0),
					idx,
					static_cast<double>(idx) / elapsed_msec * 1000.0,
					games,
					draws,
					nyugyokus,
					static_cast<double>(madeTeacherNodes) / games,
					static_cast<double>(sum_playouts) / sum_nodes,
					static_cast<double>(sum_playouts_remove) / sum_nodes,
					static_cast<double>(sum_c_dynamic) / sum_nodes,
					ss.str(),
					usi_games,
					usi_wins,
					usi_draws,
					elapsed_msec / 1000,
					std::max<s64>(0, (s64)(elapsed_msec * (1.0 - progress) / (progress * 1000))));
			int running = 0;
			for (size_t i = 0; i < group_pairs.size(); i++)
				running += group_pairs[i].Running();
			if (running == 0)
				break;
		}
	};

	while (!stopflg) {
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
		int running = 0;
		for (size_t i = 0; i < group_pairs.size(); i++)
			running += group_pairs[i].Running();
		if (running > 0)
			break;
	}
	Timer t = Timer::currentTime();
	std::thread progressThread([&progressFunc, &t] { progressFunc(t); });

	// 探索スレッド終了待機
	for (size_t i = 0; i < group_pairs.size(); i++)
		group_pairs[i].Join();

	progressThread.join();
	ifs.close();
	ofs.close();
	if (SPLIT_OPPONENT) ofs_opponent.close();
	if (OUT_MIN_HCP) ofs_minhcp.close();

	logger->info("Made {} teacher nodes in {} seconds. games:{}, draws:{}, ply/game:{}, usi_games:{}, usi_win:{}, usi_draw:{}, usi_winrate:{:.2f}%",
		madeTeacherNodes, t.elapsed() / 1000,
		games,
		draws,
		static_cast<double>(madeTeacherNodes) / games,
		usi_games,
		usi_wins,
		usi_draws,
		static_cast<double>(usi_wins) / (usi_games - usi_draws) * 100);

	logger->flush();

	// リソースの破棄はOSに任せてすぐに終了する
	std::quick_exit(0);
}

int main(int argc, char* argv[]) {
	std::string recordFileName;
	std::string outputFileName;
	vector<int> gpu_id(1);
	vector<int> batchsize(1);

	cxxopts::Options options("selfplay");
	options.positional_help("modelfile hcp output nodes playout_num gpu_id batchsize [gpu_id batchsize]*");
	try {
		options.add_options()
			("modelfile", "model file path", cxxopts::value<std::string>(model_path))
			("hcp", "initial position file", cxxopts::value<std::string>(recordFileName))
			("output", "output file path", cxxopts::value<std::string>(outputFileName))
			("nodes", "nodes", cxxopts::value<s64>(teacherNodes))
			("playout_num", "playout number", cxxopts::value<int>(playout_num))
			("gpu_id", "gpu id", cxxopts::value<int>(gpu_id[0]))
			("batchsize", "batchsize", cxxopts::value<int>(batchsize[0]))
			("positional", "", cxxopts::value<std::vector<int>>())
			("threads", "thread number", cxxopts::value<int>(threads)->default_value("2"), "num")
			("kldgain", "thread number", cxxopts::value<float>(KLDGAIN_THRESHOLD)->default_value("0.0000100"), "num")
			("random", "random move number", cxxopts::value<int>(RANDOM_MOVE)->default_value("30"), "num")
			("random_cutoff", "random cutoff value", cxxopts::value<float>(RANDOM_CUTOFF)->default_value("0.015"))
			("random_cutoff_drop", "random cutoff drop", cxxopts::value<float>(RANDOM_CUTOFF_DROP)->default_value("0.001"))
			("random_temperature", "random temperature", cxxopts::value<float>(RANDOM_TEMPERATURE)->default_value("1.40"))
			("random_temperature_final", "random temperature", cxxopts::value<float>(RANDOM_TEMPERATURE_FINAL)->default_value("0.45"))
			("random_temperature_drop", "random temperature drop", cxxopts::value<float>(RANDOM_TEMPERATURE_DROP)->default_value("0.070"))
			("train_random", "train random move", cxxopts::value<bool>(TRAIN_RANDOM)->default_value("false"))
			("random2", "random2", cxxopts::value<float>(RANDOM2)->default_value("0"))
			("min_move", "minimum move number", cxxopts::value<int>(MIN_MOVE)->default_value("45"), "num")
			("max_move", "maximum move number", cxxopts::value<int>(MAX_MOVE)->default_value("320"), "num")
			("out_max_move", "output the max move game", cxxopts::value<bool>(OUT_MAX_MOVE)->default_value("false"))
			("root_noise", "add noise to the policy prior at the root", cxxopts::value<int>(ROOT_NOISE)->default_value("3"), "per mille")
			("root_alpha", "add noise to the policy prior at the root", cxxopts::value<float>(ALPHA_D)->default_value("0.75"), "per mille")
			("winrate_count", "winrate_count", cxxopts::value<int>(WINRATE_COUNT)->default_value("5"), "num")
			("threshold", "winrate threshold", cxxopts::value<float>(WINRATE_THRESHOLD)->default_value("0.86"), "rate")
			("mate_depth", "mate search depth", cxxopts::value<uint32_t>(ROOT_MATE_SEARCH_DEPTH)->default_value("0"), "depth")
			("mate_nodes", "mate search max nodes", cxxopts::value<int64_t>(MATE_SEARCH_MAX_NODE)->default_value("100000"), "nodes")
			("c_init", "UCT parameter c_init", cxxopts::value<float>(c_init)->default_value("1.49"), "val")
			("c_base", "UCT parameter c_base", cxxopts::value<float>(c_base)->default_value("39470.0"), "val")
			("c_fpu_reduction", "UCT parameter c_fpu_reduction", cxxopts::value<float>(c_fpu_reduction)->default_value("20"), "val")
			("c_init_root", "UCT parameter c_init_root", cxxopts::value<float>(c_init_root)->default_value("1.60"), "val")
			("c_base_root", "UCT parameter c_base_root", cxxopts::value<float>(c_base_root)->default_value("39470.0"), "val")
			("temperature", "Softmax temperature", cxxopts::value<float>(temperature)->default_value("1.40"), "val")
			("root_temperature", "Softmax temperature", cxxopts::value<float>(root_temperature)->default_value("1.55"), "val")
			("reuse", "reuse sub tree", cxxopts::value<bool>(REUSE_SUBTREE)->default_value("false"))
			("nn_cache_size", "nn cache size", cxxopts::value<unsigned int>(nn_cache_size)->default_value("8388608"))
			("split_opponent", "split opponent's hcpe3", cxxopts::value<bool>(SPLIT_OPPONENT)->default_value("false"))
			("out_min_hcp", "output minimum move hcp", cxxopts::value<bool>(OUT_MIN_HCP)->default_value("false"))
			("usi_engine", "USIEngine exe path", cxxopts::value<std::string>(usi_engine_path))
			("usi_engine_num", "USIEngine number", cxxopts::value<int>(usi_engine_num)->default_value("0"), "num")
			("usi_threads", "USIEngine thread number", cxxopts::value<int>(usi_threads)->default_value("1"), "num")
			("usi_options", "USIEngine options", cxxopts::value<std::string>(usi_options))
			("usi_byoyomi", "USI byoyomi", cxxopts::value<int>(usi_byoyomi)->default_value("100000"))
			("usi_turn", "USIEngine turn", cxxopts::value<int>(usi_turn)->default_value("-1"))
			("h,help", "Print help")
			;
		options.parse_positional({ "modelfile", "hcp", "output", "nodes", "playout_num", "gpu_id", "batchsize", "positional" });

		auto result = options.parse(argc, argv);

		if (result.count("help")) {
			std::cout << options.help({}) << std::endl;
			return 0;
		}

		const size_t positional_count = result.count("positional");
		if (positional_count > 0) {
			if (positional_count % 2 == 1) {
				throw cxxopts::option_required_exception("batchsize");
			}
			auto positional = result["positional"].as<std::vector<int>>();
			for (size_t i = 0; i < positional_count; i += 2) {
				gpu_id.push_back(positional[i]);
				batchsize.push_back(positional[i + 1]);
			}
		}
	}
	catch (cxxopts::OptionException& e) {
		std::cout << options.usage() << std::endl;
		std::cerr << e.what() << std::endl;
		return 0;
	}

	if (teacherNodes <= 0) {
		cerr << "too few teacherNodes" << endl;
		return 0;
	}
	if (playout_num <= 0) {
		cerr << "too few playout_num" << endl;
		return 0;
	}
	if (threads < 0) {
		cerr << "too few threads number" << endl;
		return 0;
	}
	if (RANDOM_MOVE < 0) {
		cerr << "too few random move number" << endl;
		return 0;
	}
	if (MIN_MOVE <= 0) {
		cerr << "too few min_move" << endl;
		return 0;
	}
	if (MAX_MOVE <= MIN_MOVE) {
		cerr << "too few max_move" << endl;
		return 0;
	}
	if (MAX_MOVE >= 1000) {
		cerr << "too large max_move" << endl;
		return 0;
	}
	if (ROOT_NOISE < 0) {
		cerr << "too few root_noise" << endl;
		return 0;
	}
	if (WINRATE_THRESHOLD <= 0) {
		cerr << "too few threshold" << endl;
		return 0;
	}
	if (MATE_SEARCH_MAX_NODE < MATE_SEARCH_MIN_NODE) {
		cerr << "too few mate nodes" << endl;
		return 0;
	}
	if (usi_engine_num < 0) {
		cerr << "too few usi_engine_num" << endl;
		return 0;
	}
	if (usi_threads < 0) {
		cerr << "too few usi_threads" << endl;
		return 0;
	}

	logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
	logger->set_level(spdlog::level::trace);
	logger->info("modelfile:{} roots.hcp:{} output:{} nodes:{} playout_num:{}", model_path, recordFileName, outputFileName, teacherNodes, playout_num);

	for (size_t i = 0; i < gpu_id.size(); i++) {
		logger->info("gpu_id:{} batchsize:{}", gpu_id[i], batchsize[i]);

		if (gpu_id[i] < 0) {
			cerr << "invalid gpu id" << endl;
			return 0;
		}
		if (batchsize[i] <= 0) {
			cerr << "too few batchsize" << endl;
			return 0;
		}
	}

	logger->info("threads:{}", threads);
	logger->info("kldgain:{}", KLDGAIN_THRESHOLD);
	logger->info("random:{}", RANDOM_MOVE);
	logger->info("random_cutoff:{}", RANDOM_CUTOFF);
	logger->info("random_cutoff_drop:{}", RANDOM_CUTOFF_DROP);
	logger->info("random_temperature:{}", RANDOM_TEMPERATURE);
	logger->info("random_temperature_drop:{}", RANDOM_TEMPERATURE_DROP);
	logger->info("train_random:{}", TRAIN_RANDOM);
	logger->info("random2:{}", RANDOM2);
	logger->info("min_move:{}", MIN_MOVE);
	logger->info("max_move:{}", MAX_MOVE);
	logger->info("out_max_move:{}", OUT_MAX_MOVE);
	logger->info("root_noise:{}", ROOT_NOISE);
	logger->info("threshold:{}", WINRATE_THRESHOLD);
	logger->info("mate depth:{}", ROOT_MATE_SEARCH_DEPTH);
	logger->info("mate nodes:{}", MATE_SEARCH_MAX_NODE);
	logger->info("c_init:{}", c_init);
	logger->info("c_base:{}", c_base);
	logger->info("c_fpu_reduction:{}", c_fpu_reduction);
	logger->info("c_init_root:{}", c_init_root);
	logger->info("c_base_root:{}", c_base_root);
	logger->info("temperature:{}", temperature);
	logger->info("root_temperature:{}", root_temperature);
	logger->info("reuse:{}", REUSE_SUBTREE);
	logger->info("nn_cache_size:{}", nn_cache_size);
	if (SPLIT_OPPONENT) logger->info("split_opponent");
	if (OUT_MIN_HCP) logger->info("out_min_hcp");
	logger->info("usi_engine:{}", usi_engine_path);
	logger->info("usi_engine_num:{}", usi_engine_num);
	logger->info("usi_threads:{}", usi_threads);
	logger->info("usi_options:{}", usi_options);
	logger->info("usi_byoyomi:{}", usi_byoyomi);
	logger->info("usi_turn:{}", usi_turn);

	initTable();
	Position::initZobrist();
	HuffmanCodedPos::init();

	set_softmax_temperature(temperature);
	set_root_softmax_temperature(root_temperature);

	signal(SIGINT, sigint_handler);

	logger->info("make_teacher");
	make_teacher(recordFileName.c_str(), outputFileName.c_str(), gpu_id, batchsize);

	spdlog::drop_all();
}
