#include <iostream>
#include <algorithm>
#include <unordered_map>
#include <memory>
#include <variant>
#include <optional>
#include <tuple>
#include <format>
#include <vector>
#include <list>
#include <map>
#include <numeric>
#include <cstdint>
#include <stdexcept>
#include <functional>

enum class OrderType
{
    GoodTillCancel,
    FillAndKill
};

// CME based Side
enum class Side
{
    Buy,
    Sell
};

//Price can be negative but not quantity
using Price = std::int32_t; 
using Quantity = std::uint32_t;
using OrderId = std::uint64_t;

struct LevelInfo
{
    Price price_;
    Quantity quantity_;
};

using LevelInfos = std::vector<LevelInfo>;

class OrderbookLevelInfos
{
    public:
        OrderbookLevelInfos(const LevelInfos& bids, const LevelInfos& asks)
            : bids_ { bids }
            , asks_ { asks }
        { }
        const LevelInfos& GetBids() const {return bids_;}
        const LevelInfos& GetAsks() const {return asks_;}
    private:
        LevelInfos bids_;
        LevelInfos asks_;
};

/*
CME based order object

inital quantity vs remaining quantity
*/
class Order
{
    public:
        Order(OrderType orderType, OrderId orderId, Side side, Price price, Quantity quantity)
        : orderType_ {orderType}
        , orderId_ { orderId }
        , side_ {side}
        , price_ {price}
        , intialQuantity_ {quantity}
        , remainingQuantity_ {quantity}
    { }
    OrderId GetOrderId() const {return orderId_;}
    Side GetSide() const {return side_;}
    Price GetPrice() const {return price_;}
    OrderType GetOrderType() const { return orderType_;}
    Quantity GetInitialQuantity() const {return intialQuantity_;}
    Quantity GetRemainingQuantity() const {return remainingQuantity_;}
    Quantity GetFilledQuantity() const {return GetInitialQuantity() - GetRemainingQuantity();}
    bool IsFilled() const { return GetRemainingQuantity() == 0; }
    // lowest quantity between orders is used to fill an order
    void Fill(Quantity quantity)
    {
        if (quantity > GetRemainingQuantity())
            throw std::logic_error(std::format("Order ({}) cannot be filled for more than its remaining quantity", GetOrderId()));
        remainingQuantity_ -= quantity;
    }

    private:
        OrderType orderType_;
        OrderId orderId_;
        Side side_;
        Price price_;
        Quantity intialQuantity_;
        Quantity remainingQuantity_;
};

//List and map to store orders
using OrderPointer = std::shared_ptr<Order>;
using OrderPointers = std::list<OrderPointer>;

class OrderModify
{
    public:
        OrderModify(OrderId orderId, Side side, Price price, Quantity quantity)
            : orderId_ {orderId}
            , price_ {price}
            , side_ {side}
            , quantity_ {quantity}
        { }

        OrderId GetOrderId() const {return orderId_;}
        Price GetPrice() const {return price_;}
        Side GetSide() const {return side_;}
        Quantity GetQuantity() const {return quantity_;}

        //supporting more order types in future
        OrderPointer ToOrderPointer(OrderType type) const 
        {
            return std::make_shared<Order>(type, GetOrderId(), GetSide(), GetPrice(), GetQuantity());
        }
    private:
        OrderId orderId_;
        Price price_;
        Side side_;
        Quantity quantity_;
};

// Trade Object = TradeInfo for Bid + Ask 
struct TradeInfo
{
    OrderId orderId_;
    Price price_;
    Quantity quantity_;
};

class Trade
{
    public:
        Trade(const TradeInfo& bidTrade, const TradeInfo& askTrade)
            : bidTrade_ {bidTrade}
            , askTrade_ {askTrade}
        { }
        
        const TradeInfo& GetBidTrade() const { return bidTrade_;}
        const TradeInfo& GetAskTrade() const { return askTrade_;}
    private:
        TradeInfo bidTrade_;
        TradeInfo askTrade_;
};

// Vector of trades for one order
using Trades = std::vector<Trade>;


/**
 * Order Book
 * 
 * bids_ / asks_: { Price : list of orders at that price level }
 *   bids sorted desc, asks sorted asc, so begin() is always the best price
 *   orders within a level are kept in arrival order (price time priority)
 * orders_: { OrderId : order + iterator to its spot in the level list }
 *   gives O(1) lookup and removal for cancel / modify
 * 
 */
class Orderbook
{
    public:
        Trades AddOrder(OrderPointer order)
        { // non const because you can mutate
            if (orders_.contains(order->GetOrderId()))
                return { };

            if (order->GetOrderType() == OrderType::FillAndKill && !CanMatch(order->GetSide(), order->GetPrice()))
                return { };
            OrderPointers::iterator iterator;

            if (order->GetSide() == Side::Buy)
            {
                auto& orders = bids_[order->GetPrice()];
                orders.push_back(order);
                iterator = std::next(orders.begin(), orders.size()-1);
            } 
            else
            {
                auto& orders = asks_[order->GetPrice()];
                orders.push_back(order);
                iterator = std::next(orders.begin(), orders.size()-1);           
            }

            orders_.insert({ order->GetOrderId(), OrderEntry{ order, iterator }});
            return MatchOrders();
        }

        void CancelOrder(OrderId orderId)
        {
            if (!orders_.contains(orderId))
                return;
            const auto [order, iterator] =  orders_.at(orderId);
            orders_.erase(orderId);

            if (order->GetSide() == Side::Sell)
            {
                auto price = order->GetPrice();
                auto& orders = asks_.at(price);
                orders.erase(iterator);
                if (orders.empty())
                    asks_.erase(price);
            }
            else
            {
                auto price = order->GetPrice();
                auto& orders = bids_.at(price);
                orders.erase(iterator);
                if (orders.empty())
                    bids_.erase(price);
            }
        }

        Trades MatchOrder(OrderModify order)
        {
        
            if(!orders_.contains(order.GetOrderId()))
                return { };

            const auto [existingOrder, _] = orders_.at(order.GetOrderId());
            CancelOrder(order.GetOrderId());
            return AddOrder(order.ToOrderPointer(existingOrder->GetOrderType()));
        }

        std::size_t Size() const { return orders_.size(); }

        OrderbookLevelInfos GetorderInfos() const
        {
            LevelInfos bidInfos, askInfos;
            bidInfos.reserve(orders_.size());
            askInfos.reserve(orders_.size());

            auto CreateLevelInfos = [](Price price, const OrderPointers& orders)
            {
                return LevelInfo{ price, std::accumulate(orders.begin(), orders.end(), (Quantity)0,
                [](Quantity runningSum, const OrderPointer& order) {return runningSum + order->GetRemainingQuantity();})
                };
            };

            for (const auto& [price, orders] : bids_)
                bidInfos.push_back(CreateLevelInfos(price, orders));

            for (const auto& [price, orders] : asks_)
                askInfos.push_back(CreateLevelInfos(price, orders));

            return OrderbookLevelInfos{ bidInfos, askInfos};
        }

    private:
        struct OrderEntry
        {
            OrderPointer order_{nullptr };
            OrderPointers::iterator location_;
        };

        // Price time priority
        std::map<Price, OrderPointers, std::greater<Price>> bids_;
        std::map<Price, OrderPointers, std::less<Price>> asks_;
        std::unordered_map<OrderId, OrderEntry> orders_;
        
        //see if we can fill order
        bool CanMatch(Side side, Price price) const
        {
            if (side == Side::Buy)
            {
                if (asks_.empty())
                    return false;
                //ask with lowest price
                const auto& [bestAsk, _] = *asks_.begin();
                return price >= bestAsk;
            }
            else
            {
                if (bids_.empty())
                    return false;
                
                const auto& [bestBid, _] = *bids_.begin();
                return price <= bestBid;
            }
        }

        // see if best bids and asks can match to create trade
        Trades MatchOrders()
        {
            Trades trades;
            trades.reserve(orders_.size());

            while (true)
            {
                if (bids_.empty() || asks_.empty())
                    break;
                auto& [bidPrice, bids] = *bids_.begin();
                auto& [askPrice, asks] = *asks_.begin();

                if (bidPrice < askPrice)
                    break;

                while (bids.size() && asks.size())
                {
                    auto bid = bids.front();
                    auto ask = asks.front();

                    Quantity quantity = std::min(bid->GetRemainingQuantity(), ask->GetRemainingQuantity());

                    bid->Fill(quantity);
                    ask->Fill(quantity);

                    if (bid->IsFilled())
                    {
                        bids.pop_front();
                        orders_.erase(bid->GetOrderId());
                    }

                    if (ask->IsFilled())
                    {
                        asks.pop_front();
                        orders_.erase(ask->GetOrderId());
                    }
                    
                    // generate new trade
                    trades.push_back(Trade {
                        TradeInfo{ bid->GetOrderId(), bid->GetPrice(), quantity},
                        TradeInfo{ ask->GetOrderId(), ask->GetPrice(), quantity} 
                        });
                }

                if(bids.empty())
                    bids_.erase(bidPrice);

                if(asks.empty())
                    asks_.erase(askPrice);
            }
            
            //cancel fill or kill not filled
            if(!bids_.empty())
            {
                auto& [_, bids] = *bids_.begin();
                auto& order = bids.front();
                if (order->GetOrderType() == OrderType::FillAndKill)
                    CancelOrder(order->GetOrderId());
            }

            if(!asks_.empty())
            {
                auto& [_, asks] = *asks_.begin();
                auto& order = asks.front();
                if (order->GetOrderType() == OrderType::FillAndKill)
                    CancelOrder(order->GetOrderId());
            }

            return trades;
        }
};


int main()
{
    Orderbook orderbook;
    const OrderId orderId = 1;
    orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, orderId, Side::Buy, 100, 10));
    std::cout << orderbook.Size() << std::endl; //1
    orderbook.CancelOrder(orderId);
    std::cout << orderbook.Size() << std::endl; //0

    // Fill and kill example: buy 10 against a resting sell of 5
    const OrderId sellId = 2;
    const OrderId fillAndKillId = 3;
    orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, sellId, Side::Sell, 100, 5));
    const auto trades = orderbook.AddOrder(std::make_shared<Order>(OrderType::FillAndKill, fillAndKillId, Side::Buy, 100, 10));
    std::cout << trades.size() << std::endl; //1 (5 units filled)
    std::cout << orderbook.Size() << std::endl; //0 (unfilled 5 units of the fill and kill were cancelled)

    // Multithreading example: two threads place orders into the same orderbook.
    // A mutex makes sure only one thread touches the orderbook at a time.
    std::mutex bookMutex;
    constexpr int ordersPerThread = 5;

    auto placeOrders = [&orderbook, &bookMutex](OrderId firstId, Side side, Price startPrice)
    {
        for (int i = 0; i < ordersPerThread; ++i)
        {
            auto order = std::make_shared<Order>(OrderType::GoodTillCancel, firstId + i, side, startPrice + i, 10);
            std::lock_guard<std::mutex> lock(bookMutex);
            orderbook.AddOrder(order);
        }
    };

    // Buyer places bids at 100-104, seller places asks at 110-114 (no crossing, so no trades)
    std::thread buyer(placeOrders, 100, Side::Buy, 100);
    std::thread seller(placeOrders, 200, Side::Sell, 110);
    buyer.join();
    seller.join();

    std::cout << orderbook.Size() << std::endl; //10 (5 buys + 5 sells)
    return 0;
}
