/*
libfive: a CAD kernel for modeling with implicit functions
Copyright (C) 2017  Matt Keeter

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include <cassert>
#include <cmath>

#include "libfive/tree/cache.hpp"
#include "libfive/eval/eval_point.hpp"

namespace Kernel {

// Static class variables
std::recursive_mutex Cache::mut;
Cache Cache::_instance;

Cache::Node Cache::constant(float v)
{
    // Special-case for NaN, which can't be stored in the usual map
    if (std::isnan(v))
    {
        auto out = nan_constant.lock();
        if (out.get() == nullptr)
        {
            out.reset(new Tree::Tree_ {
                    Opcode::CONSTANT,
                    Tree::FLAG_LOCATION_AGNOSTIC,
                    0, // rank
                    v, // value
                    nullptr, // oracle
                    nullptr,
                    nullptr });
            nan_constant = out;
        }
        return out;
    }

    auto f = constants.find(v);
    if (f == constants.end())
    {
        Node out(new Tree::Tree_ {
            Opcode::CONSTANT,
            Tree::FLAG_LOCATION_AGNOSTIC,
            0, // rank
            v, // value
            nullptr, // oracle
            nullptr,
            nullptr });
        constants.insert({v, out});
        return out;
    }
    else
    {
        assert(!f->second.expired());
        return f->second.lock();
    }
}

Cache::Node Cache::operation(Opcode::Opcode op, Cache::Node lhs,
                             Cache::Node rhs, bool simplify)
{
    // These are opcodes that you're not allowed to use here
    assert(op != Opcode::CONSTANT &&
           op != Opcode::INVALID &&
           op != Opcode::ORACLE &&
           op != Opcode::LAST_OP);

    // See if we can simplify the expression, either because it's an identity
    // operation (e.g. X + 0) or a commutative expression to be balanced
    if (simplify)
    {
#define CHECK_RETURN(func) { auto t = func(op, lhs, rhs); if (t.get() != nullptr) { return t; }}
        CHECK_RETURN(checkIdentity);
        CHECK_RETURN(checkCommutative);
        CHECK_RETURN(checkAffine);
    }

    Key k(op, lhs.get(), rhs.get());

    auto found = ops.find(k);
    if (found == ops.end())
    {
        // Construct a new operation node
        Node out(new Tree::Tree_ {
            op,

            // Flags
            (uint8_t)
            (((!lhs.get() || (lhs->flags & Tree::FLAG_LOCATION_AGNOSTIC)) &&
              (!rhs.get() || (rhs->flags & Tree::FLAG_LOCATION_AGNOSTIC)) &&
               op != Opcode::VAR_X &&
               op != Opcode::VAR_Y &&
               op != Opcode::VAR_Z &&
               op != Opcode::ORACLE)
                  ? Tree::FLAG_LOCATION_AGNOSTIC : 0),

            // Rank
            std::max(lhs.get() ? lhs->rank + 1 : 0,
                     rhs.get() ? rhs->rank + 1 : 0),

            // Value
            std::nanf(""),

            // Oracle
            nullptr,

            // Arguments
            lhs,
            rhs });

        // Store a weak pointer to this new Node
        ops.insert({k, out});

        // If both sides of the operation are constant, then build up a
        // temporary Evaluator in order to get a constant value out
        // (out will be GC'd immediately when it goes out of scope)
        if ((lhs.get() || rhs.get()) &&
            (!lhs.get() || lhs->op == Opcode::CONSTANT) &&
            (!rhs.get() || rhs->op == Opcode::CONSTANT))
        {
            // Here, we construct a Tree manually to avoid a recursive loop,
            // then pass it immediately into a dummy Evaluator
            PointEvaluator e(std::make_shared<Tape>(Tree(out)));
            auto result = e.eval({0,0,0});
            return constant(result);
        }
        return out;
    }
    else
    {
        assert(!found->second.expired());
        return found->second.lock();
    }
}


Cache::Node Cache::var()
{
    return Node(new Tree::Tree_ {
        Opcode::VAR_FREE,
        Tree::FLAG_LOCATION_AGNOSTIC,
        0, // rank
        std::nanf(""), // value
        nullptr, // oracle
        nullptr,
        nullptr});
}

void Cache::del(float v)
{
    if (std::isnan(v))
    {
        assert(nan_constant.expired());
        nan_constant.reset();
    }
    else
    {
        auto c = constants.find(v);
        assert(c != constants.end());
        assert(c->second.expired());
        constants.erase(c);
    }
}

void Cache::del(Opcode::Opcode op, Node lhs, Node rhs)
{
    auto o = ops.find(Key(op, lhs.get(), rhs.get()));
    assert(o != ops.end());
    assert(o->second.expired());
    ops.erase(o);
}

std::map<Cache::Node, float> Cache::asAffine(Node n)
{
    std::map<Node, float> out;

    if (n->op == Opcode::OP_ADD)
    {
        out = asAffine(n->lhs);
        for (const auto& i : asAffine(n->rhs))
        {
            if (out.find(i.first) == out.end())
            {
                out.insert(i);
            }
            else
            {
                out[i.first] += i.second;
            }
        }
    }
    else if (n->op == Opcode::OP_SUB)
    {
        out = asAffine(n->lhs);
        for (const auto& i : asAffine(n->rhs))
        {
            if (out.find(i.first) == out.end())
            {
                out.insert({i.first, -i.second});
            }
            else
            {
                out[i.first] -= i.second;
            }
        }
    }
    else if (n->op == Opcode::OP_NEG)
    {
        for (const auto& i : asAffine(n->lhs))
        {
            out.insert({i.first, -i.second});
        }
    }
    else if (n->op == Opcode::OP_MUL)
    {
        if (n->lhs->op == Opcode::CONSTANT)
        {
            for (const auto& i : asAffine(n->rhs))
            {
                out.insert({i.first, i.second * n->lhs->value});
            }
        }
        else if (n->rhs->op == Opcode::CONSTANT)
        {
            for (const auto& i : asAffine(n->lhs))
            {
                out.insert({i.first, i.second * n->rhs->value});
            }
        }
        else
        {
            out.insert({n, 1});
        }
    }
    else if (n->op == Opcode::OP_DIV)
    {
        if (n->rhs->op == Opcode::CONSTANT)
        {
            for (const auto& i : asAffine(n->lhs))
            {
                out.insert({i.first, i.second / n->rhs->value});
            }
        }
        else
        {
            out.insert({n, 1});
        }
    }
    else if (n->op == Opcode::CONSTANT)
    {
        out.insert({constant(1), n->value});
    }
    else
    {
        out.insert({n, 1});
    }

    return out;
}

Cache::Node Cache::fromAffine(const std::map<Node, float>& ns)
{
    std::map<float, std::list<Node>> cs;
    for (const auto& n : ns)
    {
        if (cs.find(n.second) == cs.end())
        {
            cs.insert({n.second, {}});
        }
        cs.at(n.second).push_back(n.first);
    }

    std::list<std::pair<float, std::list<Node>>> pos;
    std::list<std::pair<float, std::list<Node>>> neg;
    for (const auto& c : cs)
    {
        if (c.first < 0)
        {
            neg.push_back({-c.first, c.second});
        }
        else if (c.first > 0)
        {
            pos.push_back(c);
        }
    }

    auto accumulate =
        [this](const std::list<std::pair<float, std::list<Node>>>& vs)
        {
            Node out = constant(0);
            for (const auto& v : vs)
            {
                Node cur = constant(0);
                for (const auto& n : v.second)
                {
                    cur = operation(Opcode::OP_ADD, cur, n);
                }
                out = operation(Opcode::OP_ADD, out,
                        operation(Opcode::OP_MUL, cur, constant(v.first)));
            }
            return out;
        };

    return operation(Opcode::OP_SUB, accumulate(pos), accumulate(neg));
}


Cache::Node Cache::checkIdentity(Opcode::Opcode op, Cache::Node a, Cache::Node b)
{
    if (Opcode::args(op) != 2)
    {
        return Node();
    }

    // Pull op-codes from both branches
    const auto op_a = a->op;
    const auto op_b = b->op;

    // Special cases to handle identity operations
    switch (op)
    {
        case Opcode::OP_ADD:
            if (op_a == Opcode::CONSTANT && a->value == 0)
            {
                return b;
            }
            else if (op_b == Opcode::CONSTANT && b->value == 0)
            {
                return a;
            }
            else if (op_b == Opcode::OP_NEG)
            {
                return operation(Opcode::OP_SUB, a, b->lhs);
            }
            break;
        case Opcode::OP_SUB:
            if (op_a == Opcode::CONSTANT && a->value == 0)
            {
                return operation(Opcode::OP_NEG, b);
            }
            else if (op_b == Opcode::CONSTANT && b->value == 0)
            {
                return a;
            }
            break;
        case Opcode::OP_MUL:
            if (op_a == Opcode::CONSTANT)
            {
                if (a->value == 0)
                {
                    return a;
                }
                else if (a->value == 1)
                {
                    return b;
                }
                else if (a->value == -1)
                {
                    return operation(Opcode::OP_NEG, b);
                }
            }
            if (op_b == Opcode::CONSTANT)
            {
                if (b->value == 0)
                {
                    return b;
                }
                else if (b->value == 1)
                {
                    return a;
                }
                else if (b->value == -1)
                {
                    return operation(Opcode::OP_NEG, a);
                }
            }
            else if (a == b)
            {
                return operation(Opcode::OP_SQUARE, a);
            }
            break;
        case Opcode::OP_POW:   // FALLTHROUGH
        case Opcode::OP_NTH_ROOT:
            if (op_b == Opcode::CONSTANT && b->value == 1)
            {
                return a;
            }
            break;

        default:
            break;
    }
    return Node();
}

Cache::Node Cache::checkCommutative(Opcode::Opcode op, Cache::Node a, Cache::Node b)
{
    if (Opcode::isCommutative(op))
    {
        const auto al = a->lhs ? a->lhs->rank : 0;
        const auto ar = a->rhs ? a->rhs->rank : 0;
        const auto bl = b->lhs ? b->lhs->rank : 0;
        const auto br = b->rhs ? b->rhs->rank : 0;

        if (a->op == op)
        {
            if (al > b->rank)
            {
                return operation(op, a->lhs, operation(op, a->rhs, b));
            }
            else if (ar > b->rank)
            {
                return operation(op, a->rhs, operation(op, a->lhs, b));
            }
        }
        else if (b->op == op)
        {
            if (bl > a->rank)
            {
                return operation(op, b->lhs, operation(op, b->rhs, a));
            }
            else if (br > a->rank)
            {
                return operation(op, b->rhs, operation(op, b->lhs, a));
            }
        }
    }
    return 0;
}

Cache::Node Cache::checkAffine(Opcode::Opcode op, Node a_, Node b_)
{
    if (op != Opcode::OP_ADD && op != Opcode::OP_SUB)
    {
        return Node();
    }

    auto a = asAffine(a_);
    const auto b = asAffine(b_);

    bool overlap = false;
    for (auto& k : b)
    {
        auto itr = a.find(k.first);
        if (itr != a.end())
        {
            if (op == Opcode::OP_ADD)
            {
                a.at(k.first) += k.second;
            }
            else
            {
                a.at(k.first) -= k.second;
            }
            overlap = true;
        }
        else
        {
            a.insert({k.first, op == Opcode::OP_ADD ? k.second : -k.second});
        }
    }

    return overlap ? fromAffine(a) : Node();
}

}   // namespace Kernel
