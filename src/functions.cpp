#include "pdf.h"
#include <numbers>

namespace pdf {
static double bounded(double x, double a, double b) {
    return std::clamp(x, std::min(a, b), std::max(a, b));
}
struct Calc {
    double value = 0;
    int type = 0;
    std::string op;
    std::vector<Calc> body;
};
struct CalculatorProgram {
    std::vector<Calc> code;
};
static std::vector<Calc> calculator_parse(Parser& p, int depth = 0) {
    require(depth < 64, "Calculator procedure nesting exceeded");
    std::vector<Calc> code;
    while (p.pos < p.data.size()) {
        skip_space(p);
        if (p.pos == p.data.size())
            break;
        auto c = p.data[p.pos];
        if (c == '}') {
            ++p.pos;
            break;
        }
        Calc item;
        if (c == '{') {
            ++p.pos;
            item.type = 2;
            item.body = calculator_parse(p, depth + 1);
        } else {
            auto v = parse(p);
            if (v.kind == Kind::number)
                item.value = v.number;
            else if (v.kind == Kind::boolean) {
                item.type = 1;
                item.value = v.number;
            } else {
                item.type = 3;
                item.op = v.text;
            }
        }
        code.push_back(std::move(item));
        require(code.size() < 100000, "Calculator program too large");
    }
    return code;
}
static void calculator_run(const std::vector<Calc>& code, std::vector<Calc>& stack, size_t& budget,
                           int depth = 0) {
    require(depth < 64, "Calculator call depth exceeded");
    auto pop = [&] {
        require(!stack.empty(), "Calculator stack underflow");
        Calc v = std::move(stack.back());
        stack.pop_back();
        return v;
    };
    auto number = [&] {
        Calc v = pop();
        require(v.type < 2, "Expected calculator number");
        return v.value;
    };
    auto push = [&](double n, int type = 0) {
        require(std::isfinite(n) && stack.size() < 1024, "Calculator value or stack overflow");
        Calc v;
        v.value = n;
        v.type = type;
        stack.push_back(std::move(v));
    };
    auto as_int = [](double v) {
        require(v >= INT32_MIN && v <= INT32_MAX, "Calculator integer overflow");
        return int32_t(v);
    };
    for (const auto& c : code) {
        require(budget-- > 0, "Calculator operation limit exceeded");
        if (c.type != 3) {
            require(stack.size() < 1024, "Calculator stack limit exceeded");
            stack.push_back(c);
            continue;
        }
        auto& op = c.op;
        if (op == "dup") {
            require(!stack.empty(), "Calculator stack underflow");
            stack.push_back(stack.back());
        } else if (op == "exch") {
            require(stack.size() >= 2, "Calculator stack underflow");
            std::swap(stack[stack.size() - 1], stack[stack.size() - 2]);
        } else if (op == "pop")
            pop();
        else if (op == "copy") {
            int n = as_int(number());
            require(n >= 0 && size_t(n) <= stack.size() && stack.size() + n <= 1024,
                    "Invalid calculator copy");
            size_t start = stack.size() - n;
            for (int i = 0; i < n; ++i) {
                auto v = stack[start + i];
                stack.push_back(std::move(v));
            }
        } else if (op == "index") {
            int n = as_int(number());
            require(n >= 0 && size_t(n) < stack.size(), "Invalid calculator index");
            auto v = stack[stack.size() - n - 1];
            stack.push_back(std::move(v));
        } else if (op == "roll") {
            int j = as_int(number()), n = as_int(number());
            require(n >= 0 && size_t(n) <= stack.size(), "Invalid calculator roll");
            if (n) {
                j = ((j % n) + n) % n;
                std::rotate(stack.end() - n, stack.end() - j, stack.end());
            }
        } else if (op == "if" || op == "ifelse") {
            Calc no, yes = pop();
            if (op == "ifelse") {
                no = std::move(yes);
                yes = pop();
            }
            bool cond = number() != 0;
            require(yes.type == 2 && (op == "if" || no.type == 2), "Invalid calculator procedure");
            if (cond)
                calculator_run(yes.body, stack, budget, depth + 1);
            else if (op == "ifelse")
                calculator_run(no.body, stack, budget, depth + 1);
        } else if (op == "abs")
            push(std::abs(number()));
        else if (op == "neg")
            push(-number());
        else if (op == "sqrt")
            push(std::sqrt(number()));
        else if (op == "ln")
            push(std::log(number()));
        else if (op == "log")
            push(std::log10(number()));
        else if (op == "sin")
            push(std::sin(number() * std::numbers::pi / 180));
        else if (op == "cos")
            push(std::cos(number() * std::numbers::pi / 180));
        else if (op == "floor")
            push(std::floor(number()));
        else if (op == "ceiling")
            push(std::ceil(number()));
        else if (op == "round")
            push(std::floor(number() + 0.5));
        else if (op == "truncate" || op == "cvi")
            push(std::trunc(number()));
        else if (op == "cvr")
            push(number());
        else if (op == "not") {
            Calc a = pop();
            require(a.type < 2, "Invalid calculator operand");
            push(a.type == 1 ? !bool(a.value) : ~as_int(a.value), a.type);
        } else {
            Calc bv = pop(), av = pop();
            require(av.type < 2 && bv.type < 2, "Invalid calculator operands");
            double a = av.value, b = bv.value;
            if (op == "add")
                push(a + b);
            else if (op == "sub")
                push(a - b);
            else if (op == "mul")
                push(a * b);
            else if (op == "div")
                push(a / b);
            else if (op == "idiv") {
                require(b != 0, "Calculator division by zero");
                push(std::trunc(a / b));
            } else if (op == "mod") {
                require(b != 0, "Calculator modulus by zero");
                push(std::fmod(a, b));
            } else if (op == "exp")
                push(std::pow(a, b));
            else if (op == "atan") {
                double angle = std::atan2(a, b) * 180 / std::numbers::pi;
                push(angle < 0 ? angle + 360 : angle);
            } else if (op == "eq")
                push(a == b, 1);
            else if (op == "ne")
                push(a != b, 1);
            else if (op == "gt")
                push(a > b, 1);
            else if (op == "ge")
                push(a >= b, 1);
            else if (op == "lt")
                push(a < b, 1);
            else if (op == "le")
                push(a <= b, 1);
            else if (op == "and")
                push(av.type == 1 && bv.type == 1 ? (bool(a) && bool(b)) : (as_int(a) & as_int(b)),
                     av.type == 1 && bv.type == 1 ? 1 : 0);
            else if (op == "or")
                push(av.type == 1 && bv.type == 1 ? (bool(a) || bool(b)) : (as_int(a) | as_int(b)),
                     av.type == 1 && bv.type == 1 ? 1 : 0);
            else if (op == "xor")
                push(av.type == 1 && bv.type == 1 ? (bool(a) != bool(b)) : (as_int(a) ^ as_int(b)),
                     av.type == 1 && bv.type == 1 ? 1 : 0);
            else if (op == "bitshift") {
                int shift = as_int(b);
                uint32_t n = uint32_t(as_int(a));
                push(shift >= 32 || shift <= -32 ? 0
                     : shift >= 0                ? int32_t(n << shift)
                                                 : int32_t(n >> -shift));
            } else
                fail("Unsupported calculator operator: " + op);
        }
    }
}
std::vector<double> evaluate_function(Document& doc, const Value& input_function,
                                      std::span<const double> input, int depth) {
    require(depth < 32, "Function recursion limit exceeded");
    auto& fn = resolve(doc, input_function);
    if (fn.kind == Kind::array) {
        std::vector<double> out;
        for (auto& f : fn.array) {
            auto part = evaluate_function(doc, f, input, depth + 1);
            require(out.size() + part.size() <= 32, "Too many function outputs");
            out.insert(out.end(), part.begin(), part.end());
        }
        return out;
    }
    require(fn.kind == Kind::dict, "Invalid PDF function");
    auto& domain = entry(doc, fn, "Domain");
    auto& range = entry(doc, fn, "Range");
    require(!input.empty() && input.size() <= 12 && domain.array.size() == input.size() * 2,
            "Invalid function domain");
    std::vector<double> x(input.begin(), input.end());
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = bounded(x[i], num(at(domain, i * 2)), num(at(domain, i * 2 + 1), 1));
    std::vector<double> out;
    int type = integer(get(fn, "FunctionType"), -1);
    if (type == 2) {
        require(x.size() == 1, "Exponential function must have one input");
        auto& a = entry(doc, fn, "C0");
        auto& b = entry(doc, fn, "C1");
        size_t n = std::max<size_t>(1, a.array.size());
        require(n <= 32, "Too many function outputs");
        double p = std::pow(x[0], num(get(fn, "N"), 1));
        for (size_t i = 0; i < n; ++i) {
            double c0 = num(at(a, i)), c1 = num(at(b, i), 1);
            out.push_back(c0 + p * (c1 - c0));
        }
    } else if (type == 3) {
        require(x.size() == 1, "Stitching function must have one input");
        auto& fs = entry(doc, fn, "Functions");
        auto& bounds = entry(doc, fn, "Bounds");
        auto& encode = entry(doc, fn, "Encode");
        require(!fs.array.empty() && bounds.array.size() + 1 == fs.array.size() &&
                    encode.array.size() == fs.array.size() * 2,
                "Invalid stitching function");
        size_t i = 0;
        while (i < bounds.array.size() && x[0] >= num(bounds.array[i]))
            ++i;
        double lo = i ? num(bounds.array[i - 1]) : num(at(domain, 0)),
               hi = i < bounds.array.size() ? num(bounds.array[i]) : num(at(domain, 1));
        double e0 = num(at(encode, i * 2)), e1 = num(at(encode, i * 2 + 1));
        double mapped = hi == lo ? e0 : e0 + (x[0] - lo) * (e1 - e0) / (hi - lo);
        out = evaluate_function(doc, fs.array[i], std::span(&mapped, 1), depth + 1);
    } else if (type == 0) {
        auto& sizes = entry(doc, fn, "Size");
        auto& enc = entry(doc, fn, "Encode");
        auto& dec = entry(doc, fn, "Decode");
        int bits = integer(get(fn, "BitsPerSample"));
        size_t n = range.array.size() / 2;
        require(sizes.array.size() == x.size() && n > 0 && n <= 32 &&
                    (bits == 1 || bits == 2 || bits == 4 || bits == 8 || bits == 12 || bits == 16 ||
                     bits == 24 || bits == 32),
                "Invalid sampled function");
        if (integer(get(fn, "Order"), 1) != 1)
            warn(doc, "Cubic sampled functions use linear interpolation");
        auto cached = doc.function_samples.find(fn.stream.data());
        std::shared_ptr<Bytes> samples;
        if (cached != doc.function_samples.end())
            samples = cached->second;
        else {
            samples = std::make_shared<Bytes>(decode_stream(doc, fn).bytes);
            if (samples->size() <= 64 * 1024 * 1024) {
                if (doc.function_cache_bytes + samples->size() > 64 * 1024 * 1024) {
                    doc.function_samples.clear();
                    doc.function_cache_bytes = 0;
                }
                doc.function_cache_bytes += samples->size();
                doc.function_samples[fn.stream.data()] = samples;
            }
        }
        const auto& data = *samples;
        std::vector<size_t> low(x.size()), high(x.size()), stride(x.size());
        std::vector<double> fraction(x.size());
        size_t total = n;
        for (size_t i = 0; i < x.size(); ++i) {
            int size = integer(sizes.array[i]);
            require(size > 0 && size <= 1000000 && total <= max_stream / size,
                    "Sampled function size limit exceeded");
            stride[i] = total;
            total *= size;
            double d0 = num(at(domain, 2 * i)), d1 = num(at(domain, 2 * i + 1));
            double e0 = num(at(enc, 2 * i)), e1 = num(at(enc, 2 * i + 1), size - 1);
            double pos = bounded(d0 == d1 ? e0 : e0 + (x[i] - d0) * (e1 - e0) / (d1 - d0), 0, size - 1);
            low[i] = size_t(pos);
            high[i] = std::min(low[i] + 1, size_t(size - 1));
            fraction[i] = pos - low[i];
        }
        require(uint64_t(total) * bits <= uint64_t(data.size()) * 8, "Truncated sampled function");
        out.assign(n, 0);
        for (size_t corner = 0; corner < (size_t(1) << x.size()); ++corner) {
            double weight = 1;
            size_t index = 0;
            for (size_t i = 0; i < x.size(); ++i) {
                bool upper = (corner >> i) & 1;
                weight *= upper ? fraction[i] : 1 - fraction[i];
                index += (upper ? high[i] : low[i]) * stride[i];
            }
            if (weight == 0)
                continue;
            for (size_t j = 0; j < n; ++j) {
                size_t bit = (index + j) * bits;
                uint64_t sample = 0;
                for (int k = 0; k < bits; ++k)
                    sample = (sample << 1) | ((data[(bit + k) / 8] >> (7 - (bit + k) % 8)) & 1);
                double d0 = num(at(dec, 2 * j), num(at(range, 2 * j))),
                       d1 = num(at(dec, 2 * j + 1), num(at(range, 2 * j + 1), 1));
                out[j] += weight * (d0 + double(sample) * (d1 - d0) / double((uint64_t(1) << bits) - 1));
            }
        }
    } else if (type == 4) {
        auto cached = doc.calculators.find(fn.stream.data());
        std::shared_ptr<CalculatorProgram> program;
        if (cached != doc.calculators.end())
            program = cached->second;
        else {
            auto data = decode_stream(doc, fn).bytes;
            Parser parser{data, 0, 0, false};
            program = std::make_shared<CalculatorProgram>();
            program->code = calculator_parse(parser);
            if (program->code.size() == 1 && program->code[0].type == 2) {
                auto body = std::move(program->code[0].body);
                program->code = std::move(body);
            }
            if (doc.calculators.size() >= 256)
                doc.calculators.clear();
            doc.calculators[fn.stream.data()] = program;
        }
        std::vector<Calc> stack;
        for (double v : x) {
            Calc c;
            c.value = v;
            stack.push_back(c);
        }
        size_t budget = 100000;
        calculator_run(program->code, stack, budget);
        size_t n = range.array.size() / 2;
        require(n > 0 && n <= 32 && stack.size() >= n, "Invalid calculator function result");
        for (size_t i = stack.size() - n; i < stack.size(); ++i) {
            require(stack[i].type < 2, "Calculator returned a procedure");
            out.push_back(stack[i].value);
        }
    } else
        fail("Unsupported PDF function type");
    for (size_t i = 0; i < out.size(); ++i) {
        require(std::isfinite(out[i]), "Non-finite PDF function result");
        if (range.array.size() >= 2 * i + 2)
            out[i] = bounded(out[i], num(at(range, i * 2)), num(at(range, i * 2 + 1), 1));
    }
    return out;
}
} // namespace pdf
