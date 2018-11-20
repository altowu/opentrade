#include "connection.h"

#include <unistd.h>
#include <boost/filesystem.hpp>
#include <boost/uuid/sha1.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <thread>

#include "algo.h"
#include "exchange_connectivity.h"
#include "logger.h"
#include "market_data.h"
#include "position.h"
#include "security.h"
#include "server.h"

namespace fs = boost::filesystem;

namespace opentrade {

static time_t kStartTime = GetTime();
static thread_local boost::uuids::random_generator kUuidGen;
static tbb::concurrent_unordered_map<std::string, const User*> kTokens;

std::string sha1(const std::string& str) {
  boost::uuids::detail::sha1 s;
  s.process_bytes(str.c_str(), str.size());
  unsigned int digest[5];
  s.get_digest(digest);
  std::string out;
  for (int i = 0; i < 5; ++i) {
    char tmp[17];
    snprintf(tmp, sizeof(tmp), "%08x", digest[i]);
    out += tmp;
  }
  return out;
}

template <typename T>
inline T Get(const json& j) {
  if (!j.is_number_integer()) {
    std::stringstream os;
    os << "wrong json value "
       << ": " << j << ", expect integer";
    throw std::runtime_error(os.str());
  }
  return j.get<T>();
}

template <>
inline std::string Get(const json& j) {
  if (!j.is_string()) {
    std::stringstream os;
    os << "wrong json value "
       << ": " << j << ", expect string";
    throw std::runtime_error(os.str());
  }
  return j.get<std::string>();
}

template <>
inline double Get(const json& j) {
  if (!j.is_number_float()) {
    std::stringstream os;
    os << "wrong json value "
       << ": " << j << ", expect float";
    throw std::runtime_error(os.str());
  }
  return j.get<double>();
}

template <>
inline bool Get(const json& j) {
  if (!j.is_boolean()) {
    std::stringstream os;
    os << "wrong json value "
       << ": " << j << ", expect bool";
    throw std::runtime_error(os.str());
  }
  return j.get<bool>();
}

inline double GetNum(const json& j) {
  if (!j.is_number()) {
    std::stringstream os;
    os << "wrong json value "
       << ": " << j << ", expect number";
    throw std::runtime_error(os.str());
  }
  if (j.is_number_float()) return j.get<double>();
  return j.get<int64_t>();
}

template <typename T>
static inline T ParseParamScalar(const json& j) {
  if (j.is_number_float()) return j.get<double>();
  if (j.is_number_integer()) return j.get<int64_t>();
  if (j.is_boolean()) return j.get<bool>();
  if (j.is_string()) return j.get<std::string>();
  if (j.is_object()) {
    DataSrc src;
    const Security* sec = nullptr;
    const SubAccount* acc = nullptr;
    OrderSide side = static_cast<OrderSide>(0);
    double qty = 0;
    for (auto& it : j.items()) {
      if (it.key() == "qty") {
        qty = GetNum(it.value());
      } else if (it.key() == "side") {
        auto side_str = Get<std::string>(it.value());
        if (!GetOrderSide(side_str, &side)) {
          throw std::runtime_error("Unknown order side: " + side_str);
        }
      } else if (it.key() == "src") {
        src = Get<std::string>(it.value());
      } else if (it.key() == "sec") {
        auto v = Get<int64_t>(it.value());
        sec = SecurityManager::Instance().Get(v);
        if (!sec)
          throw std::runtime_error("Unknown security id: " + std::to_string(v));
      } else if (it.key() == "acc") {
        if (it.value().is_number_integer()) {
          auto v = Get<int64_t>(it.value());
          acc = AccountManager::Instance().GetSubAccount(v);
          if (!acc)
            throw std::runtime_error("Unknown account id: " +
                                     std::to_string(v));
        } else if (it.value().is_string()) {
          auto v = Get<std::string>(it.value());
          acc = AccountManager::Instance().GetSubAccount(v);
          if (!acc) throw std::runtime_error("Unknown account: " + v);
        }
      }
    }
    auto s = SecurityTuple{src, sec, acc, side, qty};
    if (qty <= 0) {
      throw std::runtime_error("Empty quantity");
    }
    if (!side) {
      throw std::runtime_error("Empty side");
    }
    if (!sec) {
      throw std::runtime_error("Empty security");
    }
    if (!acc) {
      throw std::runtime_error("Empty account");
    }
    return s;
  }
  return {};
}

static inline ParamDef::Value ParseParamValue(const json& j) {
  if (j.is_array()) {
    ParamDef::ValueVector v;
    for (auto& it : j.items()) {
      v.push_back(ParseParamScalar<ParamDef::ValueScalar>(it.value()));
    }
    return v;
  }
  return ParseParamScalar<ParamDef::Value>(j);
}

static inline decltype(auto) ParseParams(const json& params) {
  auto m = std::make_shared<Algo::ParamMap>();
  for (auto& it : params.items()) {
    (*m.get())[it.key()] = ParseParamValue(it.value());
  }
  return m;
}

Connection::~Connection() {
  LOG_DEBUG(GetAddress() << ": Connection destructed");
}

Connection::Connection(Transport::Ptr transport,
                       std::shared_ptr<boost::asio::io_service> service)
    : transport_(transport), strand_(*service), timer_(*service) {}

void Connection::PublishMarketStatus() {
  auto& ecs = ExchangeConnectivityManager::Instance().adapters();
  for (auto& it : ecs) {
    auto& name = it.first;
    auto v = it.second->connected();
    auto it2 = ecs_.find(name);
    if (it2 == ecs_.end() || it2->second != v) {
      json j = {
          "market",
          "exchange",
          name,
          v,
      };
      ecs_[name] = v;
      Send(j.dump());
    }
  }
  auto& mds = MarketDataManager::Instance().adapters();
  for (auto& it : mds) {
    auto& name = it.first;
    auto v = it.second->connected();
    auto it2 = mds_.find(name);
    if (it2 == mds_.end() || it2->second != v) {
      json j = {
          "market",
          "data",
          name,
          v,
      };
      mds_[name] = v;
      Send(j.dump());
    }
  }
}

static inline void GetMarketData(const MarketData& md, const MarketData& md0,
                                 Security::IdType id, json* j) {
  if (md.tm == md0.tm) return;
  json j3;
  j3["t"] = md.tm;
  if (md.trade.open != md0.trade.open) j3["o"] = md.trade.open;
  if (md.trade.high != md0.trade.high) j3["h"] = md.trade.high;
  if (md.trade.low != md0.trade.low) j3["l"] = md.trade.low;
  if (md.trade.close != md0.trade.close) j3["c"] = md.trade.close;
  if (md.trade.qty != md0.trade.qty) j3["q"] = md.trade.qty;
  if (md.trade.volume != md0.trade.volume) j3["v"] = md.trade.volume;
  if (md.trade.vwap != md0.trade.vwap) j3["V"] = md.trade.vwap;
  for (auto i = 0u; i < 5u; ++i) {
    char name[3] = "a";
    auto& d0 = md0.depth[i];
    auto& d = md.depth[i];
    if (d.ask_price != d0.ask_price) {
      name[1] = '0' + i;
      j3[name] = d.ask_price;
    }
    name[0] = 'A';
    if (d.ask_size != d0.ask_size) {
      name[1] = '0' + i;
      j3[name] = d.ask_size;
    }
    name[0] = 'b';
    if (d.bid_price != d0.bid_price) {
      name[1] = '0' + i;
      j3[name] = d.bid_price;
    }
    name[0] = 'B';
    if (d.bid_size != d0.bid_size) {
      name[1] = '0' + i;
      j3[name] = d.bid_size;
    }
  }
  if (!j3.size()) return;
  json j2 = {
      id,
  };
  j2.push_back(j3);
  j->push_back(j2);
}

void Connection::PublishMarketdata() {
  if (closed_) return;
  auto self = shared_from_this();
  timer_.expires_from_now(boost::posix_time::milliseconds(1000));
  timer_.async_wait(strand_.wrap([self](auto) {
    self->PublishMarketdata();
    self->PublishMarketStatus();
    json j = {
        "md",
    };
    for (auto& pair : self->subs_) {
      auto id = pair.first;
      auto& md = MarketDataManager::Instance().Get(id);
      GetMarketData(md, pair.second.first, id, &j);
      pair.second.first = md;
    }
    if (j.size() > 1) {
      self->Send(j.dump());
    }
    if (!self->sub_pnl_) return;
    auto sub_accounts = self->user_->sub_accounts;
    for (auto& pair : PositionManager::Instance().sub_positions_) {
      auto sub_account_id = pair.first.first;
      if (sub_accounts->find(sub_account_id) == sub_accounts->end()) continue;
      auto sec_id = pair.first.second;
      auto& pnl0 = self->single_pnls_[pair.first];
      auto& pos = pair.second;
      auto x = pos.realized_pnl != pnl0.first;
      if (x || pos.unrealized_pnl != pnl0.second) {
        pnl0.first = pos.realized_pnl;
        pnl0.second = pos.unrealized_pnl;
        json j = {
            "pnl",
            sub_account_id,
            sec_id,
            pnl0.second,
        };
        if (x) j.push_back(pnl0.first);
        self->Send(j.dump());
      }
    }
    for (auto& pair : PositionManager::Instance().pnls_) {
      auto id = pair.first;
      if (sub_accounts->find(id) == sub_accounts->end()) continue;
      auto& pnl0 = self->pnls_[id];
      auto& pnl = pair.second;
      if (pnl.realized != pnl0.first || pnl.unrealized != pnl0.second) {
        pnl0.first = pnl.realized;
        pnl0.second = pnl.unrealized;
        json j = {
            "Pnl", id, GetTime(), pnl.realized, pnl.unrealized,
        };
        self->Send(j.dump());
      }
    }
  }));
}

template <typename T>
static inline bool JsonifyScala(const T& v, json* j) {
  if (auto p_val = std::get_if<bool>(&v)) {
    j->push_back("bool");
    j->push_back(*p_val);
  } else if (auto p_val = std::get_if<int64_t>(&v)) {
    j->push_back("int");
    j->push_back(*p_val);
  } else if (auto p_val = std::get_if<int32_t>(&v)) {
    j->push_back("int");
    j->push_back(*p_val);
  } else if (auto p_val = std::get_if<double>(&v)) {
    j->push_back("float");
    j->push_back(*p_val);
  } else if (auto p_val = std::get_if<std::string>(&v)) {
    j->push_back("string");
    j->push_back(*p_val);
  } else if (auto p_val = std::get_if<const char*>(&v)) {
    j->push_back("string");
    j->push_back(*p_val);
  } else if (auto p_val = std::get_if<SecurityTuple>(&v)) {
    j->push_back("security");
  } else {
    return false;
  }
  return true;
}  // namespace opentrade

static inline void Jsonify(const ParamDef::Value& v, json* j) {
  if (JsonifyScala(v, j)) return;
  if (auto p_val = std::get_if<ParamDef::ValueVector>(&v)) {
    j->push_back("vector");
    json j2;
    for (auto& v2 : *p_val) {
      json j3;
      if (JsonifyScala(v2, &j3)) j2.push_back(j3);
    }
    j->push_back(j2);
  }
}

void Connection::OnMessageAsync(const std::string& msg) {
  if (closed_) return;
  auto self = shared_from_this();
  strand_.post([self, msg]() { self->OnMessageSync(msg); });
}

void Connection::OnMessageSync(const std::string& msg,
                               const std::string& token) {
  try {
    if (msg == "h") {
      Send("h");
      return;
    }
    auto j = json::parse(msg);
    auto action = Get<std::string>(j[0]);
    if (action.empty()) {
      json j = {"error", "msg", "action", "empty action"};
      LOG_DEBUG(GetAddress() << ": " << j << '\n' << msg);
      Send(j.dump());
      return;
    }
    if (action != "login" && !user_) {
      user_ = FindInMap(kTokens, token);
      if (!user_) {
        json j = {"error", "msg", "action", "you must login first"};
        Send(j.dump());
        return;
      }
    }
    if (action == "login" || action == "validate_user") {
      OnLogin(action, j);
    } else if (action == "bod") {
      auto accs = user_->sub_accounts;
      for (auto& pair : PositionManager::Instance().bods_) {
        auto acc = pair.first.first;
        if (!user_->is_admin && accs->find(acc) == accs->end()) continue;
        auto sec_id = pair.first.second;
        auto& pos = pair.second;
        json j = {
            "bod",
            acc,
            sec_id,
            pos.qty,
            pos.avg_px,
            pos.realized_pnl,
            pos.broker_account_id,
            pos.tm,
        };
        Send(j.dump());
      }
    } else if (action == "reconnect") {
      auto name = Get<std::string>(j[1]);
      auto m = MarketDataManager::Instance().GetAdapter(name);
      if (m) {
        m->Reconnect();
        return;
      }
      auto e = ExchangeConnectivityManager::Instance().GetAdapter(name);
      if (e) {
        e->Reconnect();
        return;
      }
    } else if (action == "securities") {
      OnSecurities(j);
    } else if (action == "position") {
      OnPosition(j, msg);
    } else if (action == "offline") {
      if (j.size() > 2) {
        auto seq_algo = Get<int64_t>(j[2]);
        LOG_DEBUG(GetAddress() << ": Offline algos requested: " << seq_algo);
        AlgoManager::Instance().LoadStore(seq_algo, this);
        json j = {"offline_algos", "complete"};
        Send(j.dump());
      }
      auto seq_confirmation = Get<int64_t>(j[1]);
      LOG_DEBUG(GetAddress()
                << ": Offline confirmations requested: " << seq_confirmation);
      GlobalOrderBook::Instance().LoadStore(seq_confirmation, this);
      json j = {"offline_orders", "complete"};
      Send(j.dump());
      j = {"offline", "complete"};
      Send(j.dump());
    } else if (action == "shutdown") {
      if (!user_->is_admin) return;
      int seconds = 3;
      double interval = 1;
      if (j.size() > 1) {
        auto n = GetNum(j[1]);
        if (n > seconds) seconds = n;
      }
      if (j.size() > 2) {
        auto n = GetNum(j[2]);
        if (n > interval && n < seconds) interval = n;
      }
      Server::Stop();
      AlgoManager::Instance().Stop();
      LOG_INFO("Shutting down");
      while (seconds) {
        LOG_INFO(seconds);
        seconds -= interval;
        std::this_thread::sleep_for(
            std::chrono::milliseconds(static_cast<int>(interval * 1000)));
        GlobalOrderBook::Instance().Cancel();
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
      // to-do: safe exit
      if (system(("kill -9 " + std::to_string(getpid())).c_str())) return;
    } else if (action == "cancel") {
      auto id = Get<int64_t>(j[1]);
      auto ord = GlobalOrderBook::Instance().Get(id);
      if (!ord) {
        json j = {"error", "cancel", "order id",
                  "Invalid order id: " + std::to_string(id)};
        LOG_DEBUG(GetAddress() << ": " << j << '\n' << msg);
        Send(j.dump());
        return;
      }
      ExchangeConnectivityManager::Instance().Cancel(*ord);
    } else if (action == "order") {
      OnOrder(j, msg);
    } else if (action == "algo") {
      OnAlgo(j, msg);
    } else if (action == "pnl") {
      auto tm0 = 0l;
      if (j.size() >= 2) tm0 = Get<int64_t>(j[1]);
      tm0 = std::max(GetTime() - 24 * 3600, tm0);
      for (auto& pair : PositionManager::Instance().pnls_) {
        auto id = pair.first;
        auto sub_accounts = user_->sub_accounts;
        if (sub_accounts->find(id) == sub_accounts->end()) continue;
        auto path = kStorePath / ("pnl-" + std::to_string(id));
        std::ifstream f(path.c_str());
        const int LINE_LENGTH = 100;
        char str[LINE_LENGTH];
        json j2;
        while (f.getline(str, LINE_LENGTH)) {
          int tm;
          double a, b;
          if (3 == sscanf(str, "%d %lf %lf", &tm, &a, &b)) {
            if (tm <= tm0) continue;
            json j = {tm, a, b};
            j2.push_back(j);
          }
        }
        if (j2.size()) {
          json j = {"Pnl", id, j2};
          Send(j.dump());
        }
      }
      sub_pnl_ = true;
    } else if (action == "sub") {
      json jout = {"md"};
      for (auto i = 1u; i < j.size(); ++i) {
        auto id = Get<int64_t>(j[i]);
        auto& s = subs_[id];
        auto sec = SecurityManager::Instance().Get(id);
        if (sec) {
          auto& md = MarketDataManager::Instance().Get(*sec);
          GetMarketData(md, s.first, id, &jout);
          s.first = md;
          s.second += 1;
        }
      }
      if (jout.size() > 1) {
        Send(jout.dump());
      }
    } else if (action == "unsub") {
      for (auto i = 1u; i < j.size(); ++i) {
        auto id = Get<int64_t>(j[i]);
        auto it = subs_.find(id);
        if (it == subs_.end()) return;
        it->second.second -= 1;
        if (it->second.second <= 0) subs_.erase(it);
      }
    } else if (action == "algoFile") {
      auto fn = Get<std::string>(j[1]);
      auto path = kAlgoPath / fn;
      json j = {action, fn};
      std::ifstream is(path.string());
      if (is.good()) {
        std::stringstream buffer;
        buffer << is.rdbuf();
        j.push_back(buffer.str());
      } else {
        j.push_back(nullptr);
        j.push_back("Not found");
      }
      Send(j.dump());
    } else if (action == "deleteAlgoFile") {
      auto fn = Get<std::string>(j[1]);
      auto path = kAlgoPath / fn;
      json j = {action, fn};
      try {
        fs::remove(path);
      } catch (const fs::filesystem_error& err) {
        j.push_back(err.what());
      }
      Send(j.dump());
    } else if (action == "saveAlgoFile") {
      auto fn = Get<std::string>(j[1]);
      auto text = Get<std::string>(j[2]);
      auto path = kAlgoPath / fn;
      json j = {action, fn};
      std::ofstream os(path.string());
      if (os.good()) {
        os << text;
      } else {
        j.push_back("Can not write");
      }
      Send(j.dump());
    }
  } catch (nlohmann::detail::parse_error& e) {
    LOG_DEBUG(GetAddress() << ": invalid json string: " << msg);
    json j = {"error", "json", msg, "invalid json string"};
    Send(j.dump());
  } catch (nlohmann::detail::exception& e) {
    LOG_DEBUG(GetAddress() << ": json error: " << e.what() << ", " << msg);
    std::string error = "json error: ";
    error += e.what();
    json j = {"error", "json", msg, error};
    Send(j.dump());
  } catch (std::exception& e) {
    LOG_DEBUG(GetAddress() << ": Connection::OnMessage: " << e.what() << ", "
                           << msg);
    json j = {"error", "Connection::OnMessage", msg, e.what()};
    Send(j.dump());
  }
}

void Connection::Send(Confirmation::Ptr cm) {
  if (closed_) return;
  if (!user_) return;
  if (user_->sub_accounts->find(cm->order->sub_account->id) ==
      user_->sub_accounts->end())
    return;
  auto self = shared_from_this();
  strand_.post([self, cm]() { self->Send(*cm.get(), false); });
}

void Connection::Send(const Algo& algo, const std::string& status,
                      const std::string& body, uint32_t seq) {
  if (closed_) return;
  if (!user_ || user_->id != algo.user().id) return;
  auto self = shared_from_this();
  strand_.post([self, &algo, status, body, seq]() {
    self->Send(algo.id(), GetTime(), algo.token(), algo.name(), status, body,
               seq, false);
  });
}

void Connection::Send(Algo::IdType id, time_t tm, const std::string& token,
                      const std::string& name, const std::string& status,
                      const std::string& body, uint32_t seq, bool offline) {
  json j = {
      offline ? "Algo" : "algo", seq, id, tm, token, name, status, body,
  };
  Send(j.dump());
}

static inline const char* GetSide(OrderSide c) {
  auto side = "";
  switch (c) {
    case kBuy:
      side = "buy";
      break;
    case kSell:
      side = "sell";
      break;
    case kShort:
      side = "short";
      break;
    default:
      break;
  }
  return side;
}

static inline const char* GetType(OrderType c) {
  auto type = "";
  switch (c) {
    case kLimit:
      type = "limit";
      break;
    case kMarket:
      type = "market";
      break;
    case kStop:
      type = "stop";
      break;
    case kStopLimit:
      type = "stop_limit";
      break;
    case kOTC:
      type = "otc";
      break;
    default:
      break;
  }
  return type;
}

static inline const char* GetTif(TimeInForce c) {
  auto tif = "";
  switch (c) {
    case kDay:
      tif = "Day";
      break;
    case kImmediateOrCancel:
      tif = "IOC";
      break;
    case kGoodTillCancel:
      tif = "GTC";
      break;
    case kAtTheOpening:
      tif = "OPG";
      break;
    case kFillOrKill:
      tif = "FOK";
      break;
    case kGoodTillCrossing:
      tif = "GTX";
      break;
    default:
      break;
  }
  return tif;
}

void Connection::Send(const Confirmation& cm, bool offline) {
  assert(cm.order);
  auto cmd = offline ? "Order" : "order";
  json j = {
      cmd,
      cm.order->id,
      cm.transaction_time / 1000000,
      cm.seq,
  };
  const char* status = nullptr;
  switch (cm.exec_type) {
    case kUnconfirmedNew:
      status = "unconfirmed";
      j.push_back(status);
      j.push_back(cm.order->sec->id);
      j.push_back(cm.order->algo_id);
      j.push_back(cm.order->user->id);
      j.push_back(cm.order->sub_account->id);
      j.push_back(cm.order->broker_account->id);
      j.push_back(cm.order->qty);
      j.push_back(cm.order->price);
      j.push_back(GetSide(cm.order->side));
      j.push_back(GetType(cm.order->type));
      j.push_back(GetTif(cm.order->tif));
      break;

    case kPendingNew:
      status = "pending";
    case kPendingCancel:
      if (!status) status = "pending_cancel";
    case kNew:
      if (!status) status = "new";
    case kCanceled:
      if (!status) status = "cancelled";
      j.push_back(status);
      if (cm.exec_type == kNew) {
        j.push_back(cm.order_id);
      }
      if (!cm.text.empty()) {
        j.push_back(cm.text);
      }
      break;

    case kFilled:
      status = "filled";
    case kPartiallyFilled:
      if (!status) status = "partial";
      j.push_back(status);
      j.push_back(cm.last_shares);
      j.push_back(cm.last_px);
      j.push_back(cm.exec_id);
      if (cm.exec_trans_type == kTransNew)
        j.push_back("new");
      else if (cm.exec_trans_type == kTransCancel)
        j.push_back("cancel");
      else
        return;
      break;

    case kRejected:
      status = "new_rejected";
    case kCancelRejected:
      if (!status) status = "cancel_rejected";
    case kRiskRejected:
      if (!status) status = "risk_rejected";
      j.push_back(status);
      j.push_back(cm.text);
      if (cm.exec_type == kRiskRejected) {
        j.push_back(cm.order->sec->id);
        j.push_back(cm.order->algo_id);
        j.push_back(cm.order->user->id);
        j.push_back(cm.order->sub_account->id);
        j.push_back(cm.order->qty);
        j.push_back(cm.order->price);
        j.push_back(GetSide(cm.order->side));
        j.push_back(GetType(cm.order->type));
        j.push_back(GetTif(cm.order->tif));
        if (cm.order->orig_id) {
          j.push_back(cm.order->orig_id);
        }
      }
      break;

    default:
      return;
      break;
  }
  Send(j.dump());
}

void Connection::OnPosition(const json& j, const std::string& msg) {
  auto security_id = Get<int64_t>(j[1]);
  auto sec = SecurityManager::Instance().Get(security_id);
  if (!sec) {
    json j = {
        "error",
        "position",
        "security id",
        "Invalid security id: " + security_id,
    };
    LOG_DEBUG(GetAddress() << ": " << j << '\n' << msg);
    Send(j.dump());
    return;
  }
  auto acc_name = Get<std::string>(j[2]);
  auto acc = AccountManager::Instance().GetSubAccount(acc_name);
  if (!acc) {
    json j = {"error", "position", "account name",
              "Invalid account name: " + acc_name};
    LOG_DEBUG(GetAddress() << ": " << j << '\n' << msg);
    Send(j.dump());
    return;
  }

  const Position* p;
  bool broker = j.size() > 3 && Get<bool>(j[3]);
  if (broker) {
    auto broker_acc = acc->GetBroker(*sec);
    if (!broker_acc) {
      json j = {"error", "position", "account name",
                "Can not find broker for this account and security pair"};
      LOG_DEBUG(GetAddress() << ": " << j << '\n' << msg);
      Send(j.dump());
      return;
    }
    p = &PositionManager::Instance().Get(*broker_acc, *sec);
  } else {
    p = &PositionManager::Instance().Get(*acc, *sec);
  }
  json out = {
      "position",
      {{"qty", p->qty},
       {"avg_px", p->avg_px},
       {"unrealized_pnl", p->unrealized_pnl},
       {"realized_pnl", p->realized_pnl},
       {"total_bought_qty", p->total_bought_qty},
       {"total_sold_qty", p->total_sold_qty},
       {"total_outstanding_buy_qty", p->total_outstanding_buy_qty},
       {"total_outstanding_sell_qty", p->total_outstanding_sell_qty},
       {"total_outstanding_sell_qty", p->total_outstanding_sell_qty}},
  };
  Send(j.dump());
}

void Connection::OnAlgo(const json& j, const std::string& msg) {
  auto action = Get<std::string>(j[1]);
  if (action == "cancel") {
    if (j[2].is_string()) {
      AlgoManager::Instance().Stop(Get<std::string>(j[2]));
      return;
    }
    AlgoManager::Instance().Stop(Get<int64_t>(j[2]));
  } else if (action == "modify") {
    auto params = ParseParams(j[3]);
    if (j[2].is_string()) {
      AlgoManager::Instance().Modify(Get<std::string>(j[2]), params);
      return;
    }
    AlgoManager::Instance().Modify(Get<int64_t>(j[2]), params);
  } else if (action == "new" || action == "test") {
    auto algo_name = Get<std::string>(j[2]);
    auto token = Get<std::string>(j[3]);
    auto algo = AlgoManager::Instance().Get(token);
    if (algo) {
      json j = {
          "error",
          "algo",
          "duplicate token",
          token,
      };
      LOG_DEBUG(GetAddress() << ": " << j << '\n' << msg);
      Send(j.dump());
      return;
    }
    try {
      Algo::ParamMapPtr params;
      if (action == "new") {
        params = ParseParams(j[4]);
        for (auto& pair : *params) {
          if (auto pval = std::get_if<SecurityTuple>(&pair.second)) {
            auto acc = pval->acc;
            auto accs = user_->sub_accounts;
            if (accs->find(acc->id) == accs->end()) {
              throw std::runtime_error("No permission to trade with account: " +
                                       std::string(acc->name));
            }
          }
        }
      } else if (token.size()) {
        test_algo_tokens_.insert(token);
      }
      std::stringstream ss;
      ss << j[4];
      if (!AlgoManager::Instance().Spawn(params, algo_name, *user_, ss.str(),
                                         token) &&
          params) {
        throw std::runtime_error("Unknown algo name: " + algo_name);
      }
    } catch (const std::runtime_error& err) {
      LOG_DEBUG(GetAddress() << ": " << err.what() << '\n' << msg);
      json j = {"error", "algo", "invalid params", token, err.what()};
      Send(j.dump());
    }
  } else {
    json j = {"error", "algo", "invalid action", action};
    Send(j.dump());
  }
}

void Connection::OnOrder(const json& j, const std::string& msg) {
  auto security_id = Get<int64_t>(j[1]);
  auto sub_account = Get<std::string>(j[2]);
  auto acc = AccountManager::Instance().GetSubAccount(sub_account);
  if (!acc) {
    json j = {"error", "order", "sub_account",
              "Invalid sub_account: " + sub_account};
    LOG_DEBUG(GetAddress() << ": " << j << '\n' << msg);
    Send(j.dump());
    return;
  }
  auto side_str = Get<std::string>(j[3]);
  auto type_str = Get<std::string>(j[4]);
  auto tif_str = Get<std::string>(j[5]);
  auto qty = GetNum(j[6]);
  auto px = GetNum(j[7]);
  auto stop_price = GetNum(j[8]);
  Contract c;
  c.qty = qty;
  c.price = px;
  c.sec = SecurityManager::Instance().Get(security_id);
  c.stop_price = stop_price;
  if (!c.sec) {
    json j = {
        "error",
        "order",
        "security id",
        "Invalid security id: " + security_id,
    };
    LOG_DEBUG(GetAddress() << ": " << j << '\n' << msg);
    Send(j.dump());
    return;
  }
  c.sub_account = acc;
  if (!GetOrderSide(side_str, &c.side)) {
    json j = {
        "error",
        "order",
        "side",
        "Invalid side: " + side_str,
    };
    LOG_DEBUG(GetAddress() << ": " << j << '\n' << msg);
    Send(j.dump());
    return;
  }
  if (!strcasecmp(type_str.c_str(), "market"))
    c.type = kMarket;
  else if (!strcasecmp(type_str.c_str(), "stop"))
    c.type = kStop;
  else if (!strcasecmp(type_str.c_str(), "stop limit"))
    c.type = kStopLimit;
  else if (!strcasecmp(type_str.c_str(), "otc"))
    c.type = kOTC;
  if (c.stop_price <= 0 && (c.type == kStop || c.type == kStopLimit)) {
    json j = {
        "error",
        "order",
        "stop price",
        "Miss stop price for stop order",
    };
    LOG_DEBUG(GetAddress() << ": " << j << '\n' << msg);
    Send(j.dump());
    return;
  }
  if (!strcasecmp(tif_str.c_str(), "GTC"))
    c.tif = kGoodTillCancel;
  else if (!strcasecmp(tif_str.c_str(), "OPG"))
    c.tif = kAtTheOpening;
  else if (!strcasecmp(tif_str.c_str(), "IOC"))
    c.tif = kImmediateOrCancel;
  else if (!strcasecmp(tif_str.c_str(), "FOK"))
    c.tif = kFillOrKill;
  else if (!strcasecmp(tif_str.c_str(), "GTX"))
    c.tif = kGoodTillCrossing;
  auto ord = new Order{};
  (Contract&)* ord = c;
  ord->user = user_;
  ExchangeConnectivityManager::Instance().Place(ord);
}

void Connection::OnSecurities(const json& j) {
  LOG_DEBUG(GetAddress() << ": Securities requested");
  auto& secs = SecurityManager::Instance().securities();
  json out;
  for (auto& pair : secs) {
    auto s = pair.second;
    if (user_->is_admin) {
      json j = {
          "security",
          s->id,
          s->symbol,
          s->exchange->name,
          s->type,
          s->multiplier,
          s->close_price,
          s->rate,
          s->currency,
          s->adv20,
          s->market_cap,
          std::to_string(s->sector),
          std::to_string(s->industry_group),
          std::to_string(s->industry),
          std::to_string(s->sub_industry),
          s->local_symbol,
          s->bbgid,
          s->cusip,
          s->sedol,
          s->isin,
      };
      if (transport_->stateless) {
        out.push_back(j);
      } else {
        Send(j.dump());
      }
    } else {
      json j = {
          "security", s->id,       s->symbol,     s->exchange->name,
          s->type,    s->lot_size, s->multiplier,
      };
      if (transport_->stateless) {
        out.push_back(j);
      } else {
        Send(j.dump());
      }
    }
  }
  if (transport_->stateless) {
    Send(out.dump());
    return;
  }
  out = {"securities", "complete"};
  Send(out.dump());
}

void Connection::OnLogin(const std::string& action, const json& j) {
  auto name = Get<std::string>(j[1]);
  auto password = sha1(Get<std::string>(j[2]));
  auto user = AccountManager::Instance().GetUser(name);
  std::string state;
  if (!user)
    state = "unknown user";
  else if (password != user->password)
    state = "wrong password";
  else if (user->is_disabled)
    state = "disabled";
  else
    state = "ok";
  if (action == "validate_user") {
    auto token = Get<int64_t>(j[3]);
    json j = {
        "user_validation",
        state == "ok" ? user->id : 0,
        token,
    };
    Send(j.dump());
    return;
  }
  if (state != "ok") {
    json j = {
        "connection",
        state,
    };
    Send(j.dump());
    return;
  }
  auto token = boost::uuids::to_string(kUuidGen());
  kTokens[token] = user;
  json out = {
      "connection",
      state,
      {{"session", PositionManager::Instance().session()},
       {"userId", user->id},
       {"startTime", kStartTime},
       {"sessionToken", token},
       {"securitiesCheckSum", SecurityManager::Instance().check_sum()}},
  };
  Send(out.dump());
  if (!user_ && !transport_->stateless) {
    user_ = user;
    PublishMarketdata();
    auto accs = user->sub_accounts;
    for (auto& pair : *accs) {
      json j = {
          "sub_account",
          pair.first,
          pair.second->name,
      };
      Send(j.dump());
    }
    if (user->is_admin) {
      for (auto& pair : AccountManager::Instance().users_) {
        for (auto& pair2 : *pair.second->sub_accounts) {
          json j = {
              "user_sub_account",
              pair.first,
              pair2.first,
              pair2.second->name,
          };
          Send(j.dump());
        }
      }
    }
    for (auto& pair : AccountManager::Instance().broker_accounts_) {
      json j = {
          "broker_account",
          pair.first,
          pair.second->name,
      };
      Send(j.dump());
    }
    for (auto& pair : AlgoManager::Instance().adapters()) {
      auto& params = pair.second->GetParamDefs();
      json j = {
          "algo_def",
          pair.second->name(),
      };
      for (auto& p : params) {
        json j2 = {
            p.name,
        };
        Jsonify(p.default_value, &j2);
        j2.push_back(p.required);
        j2.push_back(p.min_value);
        j2.push_back(p.max_value);
        j2.push_back(p.precision);
        j.push_back(j2);
      }
      Send(j.dump());
    }
    json files;
    if (fs::is_directory(kAlgoPath)) {
      for (auto& entry :
           boost::make_iterator_range(fs::directory_iterator(kAlgoPath), {})) {
        auto tmp = entry.path();
        auto fn = tmp.filename().string();
        if (fn[0] == '_' || fn[0] == '.') continue;
        files.push_back(fn);
      }
    }
    if (files.size() > 0) {
      json j = {"algoFiles", files};
      Send(j.dump());
    }
  }
}

void Connection::SendTestMsg(const std::string& token, const std::string& msg,
                             bool stopped) {
  if (closed_) return;
  if (test_algo_tokens_.find(token) == test_algo_tokens_.end()) return;
  auto self = shared_from_this();
  strand_.post([self, msg, stopped, token]() {
    json j = {"test_msg", msg};
    self->Send(j.dump());
    if (stopped) {
      json out = {"test_done", token};
      self->Send(out.dump());
    }
  });
}

}  // namespace opentrade
