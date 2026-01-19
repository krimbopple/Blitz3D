#include "preprocessor.h"
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <iostream>
#include <ctime>

Preprocessor::Preprocessor() {
    define("__LINE__");
    define("__FILE__");
    define("__DATE__");
    define("__TIME__");
}

std::string Preprocessor::process(const std::string& filename, bool debug) {
    std::ifstream file(filename.c_str());
    if (!file.is_open()) {
        throw std::runtime_error("Cannot open file: " + filename);
    }

    if (std::find(includedFiles_.begin(), includedFiles_.end(), filename) != includedFiles_.end()) {
        return "";
    }

    includedFiles_.push_back(filename);
    std::ostringstream output;
    std::string line;
    int lineNumber = 0;

    conditionStack_.push(true);
    skipStack_.push(false);

    while (std::getline(file, line)) {
        lineNumber++;

        while (!line.empty() && line[line.size() - 1] == '\\') {
            line.erase(line.size() - 1);
            std::string nextLine;
            if (std::getline(file, nextLine)) {
                line += nextLine;
                lineNumber++;
            }
        }

        std::string trimmedLine = trim(line);

        if (!trimmedLine.empty() && trimmedLine.size() >= 2 &&
            trimmedLine[0] == '_' && trimmedLine[1] == '#') {
            std::string directive = trimmedLine.substr(2);

            size_t space = directive.find_first_of(" \t");
            std::string cmd = (space != std::string::npos) ?
                directive.substr(0, space) : directive;
            std::string args = (space != std::string::npos) ?
                trim(directive.substr(space + 1)) : "";

            std::transform(cmd.begin(), cmd.end(), cmd.begin(), ::tolower);

            bool processed = false;
            if (cmd == "define") {
                processed = processDefine(args);
            }
            else if (cmd == "undef") {
                processed = processUndef(args);
            }
            else if (cmd == "if") {
                processed = processIf(args);
            }
            else if (cmd == "ifdef") {
                processed = processIfdef(args, true);
            }
            else if (cmd == "ifndef") {
                processed = processIfdef(args, false);
            }
            else if (cmd == "elif") {
                processed = processElif(args);
            }
            else if (cmd == "else") {
                processed = processElse(args);
            }
            else if (cmd == "endif") {
                processed = processEndif(args);
            }
            else if (cmd == "include") {
                if (!args.empty() && args[0] == '"' && args[args.size() - 1] == '"') {
                    std::string includeFile = args.substr(1, args.size() - 2);

                    std::string basePath = filename.substr(0, filename.find_last_of("/\\") + 1);
                    std::string fullPath = basePath + includeFile;

                    output << process(fullPath, debug);
                }
                processed = true;
            }
            else if (cmd == "error") {
                if (!skipStack_.top()) {
                    throw std::runtime_error("Preprocessor error: " + args);
                }
                processed = true;
            }
            else if (cmd == "warning") {
                if (!skipStack_.top()) {
                    std::cerr << "Preprocessor warning: " << args << std::endl;
                }
                processed = true;
            }

            if (processed) {
                continue;
            }
        }

        bool skip = false;
        std::stack<bool> tempStack = skipStack_;
        while (!tempStack.empty()) {
            if (tempStack.top()) {
                skip = true;
                break;
            }
            tempStack.pop();
        }

        if (!skip) {
            std::string expanded = line;

            size_t pos = expanded.find("__LINE__");
            while (pos != std::string::npos) {
                expanded.replace(pos, 8, std::to_string(lineNumber));
                pos = expanded.find("__LINE__", pos + 1);
            }

            pos = expanded.find("__FILE__");
            while (pos != std::string::npos) {
                expanded.replace(pos, 8, "\"" + filename + "\"");
                pos = expanded.find("__FILE__", pos + 1);
            }

            if (expanded.find("__DATE__") != std::string::npos) {
                time_t now = time(0);
                char buffer[80];
                strftime(buffer, sizeof(buffer), "\"%b %d %Y\"", localtime(&now));
                std::string dateStr = buffer;

                pos = expanded.find("__DATE__");
                while (pos != std::string::npos) {
                    expanded.replace(pos, 8, dateStr);
                    pos = expanded.find("__DATE__", pos + 1);
                }
            }

            if (expanded.find("__TIME__") != std::string::npos) {
                time_t now = time(0);
                char buffer[80];
                strftime(buffer, sizeof(buffer), "\"%H:%M:%S\"", localtime(&now));
                std::string timeStr = buffer;

                pos = expanded.find("__TIME__");
                while (pos != std::string::npos) {
                    expanded.replace(pos, 8, timeStr);
                    pos = expanded.find("__TIME__", pos + 1);
                }
            }

            expanded = expandMacros(expanded);
            output << expanded << "\n";
        }
    }

    conditionStack_.pop();
    skipStack_.pop();

    includedFiles_.pop_back();
    return output.str();
}

void Preprocessor::define(const std::string& name, const std::string& value) {
    Macro macro;
    macro.name = name;
    macro.value = value;
    macro.is_function = false;
    macros_[name] = macro;
}

void Preprocessor::define(const std::string& name, const std::vector<std::string>& params, const std::string& value) {
    Macro macro;
    macro.name = name;
    macro.value = value;
    macro.params = params;
    macro.is_function = true;
    macros_[name] = macro;
}

void Preprocessor::undef(const std::string& name) {
    macros_.erase(name);
}

bool Preprocessor::isDefined(const std::string& name) {
    return macros_.find(name) != macros_.end();
}

bool Preprocessor::processDefine(const std::string& line) {
    if (line.empty()) return false;

    if (!skipStack_.empty() && skipStack_.top()) {
        return true;
    }

    size_t nameStart = 0;
    while (nameStart < line.length() && std::isspace(line[nameStart])) nameStart++;
    if (nameStart >= line.length()) return false;

    size_t nameEnd = nameStart;
    while (nameEnd < line.length() &&
        (std::isalnum(line[nameEnd]) || line[nameEnd] == '_')) nameEnd++;

    std::string name = line.substr(nameStart, nameEnd - nameStart);
    if (name.empty()) return false;

    std::string rest = trim(line.substr(nameEnd));

    Macro macro;
    macro.name = name;
    macro.is_function = false;

    if (!rest.empty() && rest[0] == '(') {
        macro.is_function = true;
        size_t paramsEnd = rest.find(')');
        if (paramsEnd == std::string::npos) {
            return false;
        }

        std::string paramsStr = rest.substr(1, paramsEnd - 1);
        std::string value = trim(rest.substr(paramsEnd + 1));

        std::istringstream paramStream(paramsStr);
        std::string param;
        while (std::getline(paramStream, param, ',')) {
            macro.params.push_back(trim(param));
        }

        macro.value = value;
    }
    else {
        macro.value = rest;
    }

    macros_[name] = macro;
    return true;
}

bool Preprocessor::processUndef(const std::string& name) {
    if (skipStack_.top()) return true;

    std::string macroName = trim(name);
    if (!macroName.empty()) {
        macros_.erase(macroName);
    }
    return true;
}

bool Preprocessor::processIf(const std::string& expr) {
    bool condition = evaluateCondition(expr);
    conditionStack_.push(condition);
    skipStack_.push(!condition);
    return true;
}

bool Preprocessor::processIfdef(const std::string& name, bool checkDefined) {
    std::string macroName = trim(name);
    bool defined = isDefined(macroName);
    bool condition = checkDefined ? defined : !defined;
    conditionStack_.push(condition);
    skipStack_.push(!condition);
    return true;
}

bool Preprocessor::processElif(const std::string& expr) {
    if (conditionStack_.empty()) return false;

    bool hadTrue = conditionStack_.top();
    conditionStack_.pop();
    skipStack_.pop();

    if (hadTrue) {
        conditionStack_.push(true);
        skipStack_.push(true);
    }
    else {
        bool condition = evaluateCondition(expr);
        conditionStack_.push(condition);
        skipStack_.push(!condition);
    }

    return true;
}

bool Preprocessor::processElse(const std::string& /*args*/) {
    if (conditionStack_.empty()) return false;

    bool previousCondition = conditionStack_.top();
    conditionStack_.pop();
    skipStack_.pop();

    // previous condition was true skip else block
    // previous condition was false include else block
    conditionStack_.push(true);
    skipStack_.push(previousCondition);
    return true;
}

bool Preprocessor::processEndif(const std::string& /*args*/) {
    if (conditionStack_.empty()) return false;

    conditionStack_.pop();
    skipStack_.pop();
    return true;
}

bool Preprocessor::evaluateCondition(const std::string& expr) {
    if (expr.empty()) return false;

    std::string processed = expandMacros(expr);
    processed = trim(processed);

    if (processed.find("defined(") != std::string::npos) {
        size_t start = processed.find("defined(");
        size_t end = processed.find(')', start);
        if (end != std::string::npos) {
            std::string macroName = processed.substr(start + 8, end - start - 8);
            macroName = trim(macroName);
            bool isDef = isDefined(macroName);

            std::string replacement = isDef ? "1" : "0";
            processed.replace(start, end - start + 1, replacement);
        }
    }

    std::istringstream iss(processed);
    int result = 0;
    char op = '+';
    int value;

    while (iss >> value) {
        if (op == '+') result += value;
        else if (op == '-') result -= value;
        else if (op == '*') result *= value;
        else if (op == '/') if (value != 0) result /= value;

        if (!(iss >> op)) break;
    }

    return result != 0;
}

std::string Preprocessor::expandMacros(const std::string& input) {
    std::string result = input;

    bool changed;
    do {
        changed = false;

        for (std::map<std::string, Macro>::const_iterator it = macros_.begin();
            it != macros_.end(); ++it) {
            const std::string& name = it->first;
            const Macro& macro = it->second;

            size_t pos = result.find(name);
            while (pos != std::string::npos) {
                bool isWord = true;
                if (pos > 0 && (std::isalnum(result[pos - 1]) || result[pos - 1] == '_')) {
                    isWord = false;
                }
                if (pos + name.length() < result.length() &&
                    (std::isalnum(result[pos + name.length()]) || result[pos + name.length()] == '_')) {
                    isWord = false;
                }

                if (isWord) {
                    if (macro.is_function) {
                        if (pos + name.length() < result.length() &&
                            result[pos + name.length()] == '(') {
                            size_t parenStart = pos + name.length();
                            size_t parenEnd = result.find(')', parenStart);
                            if (parenEnd != std::string::npos) {
                                std::string argsStr = result.substr(parenStart + 1, parenEnd - parenStart - 1);
                                std::vector<std::string> args;

                                std::istringstream argStream(argsStr);
                                std::string arg;
                                int depth = 0;
                                std::string currentArg;

                                for (size_t i = 0; i < argsStr.length(); i++) {
                                    char c = argsStr[i];
                                    if (c == '(') depth++;
                                    else if (c == ')') depth--;
                                    else if (c == ',' && depth == 0) {
                                        args.push_back(trim(currentArg));
                                        currentArg.clear();
                                        continue;
                                    }
                                    currentArg += c;
                                }
                                if (!currentArg.empty()) {
                                    args.push_back(trim(currentArg));
                                }

                                std::string expanded = macro.value;
                                for (size_t i = 0; i < macro.params.size() && i < args.size(); i++) {
                                    size_t paramPos = expanded.find(macro.params[i]);
                                    while (paramPos != std::string::npos) {
                                        bool isParam = true;
                                        if (paramPos > 0 &&
                                            (std::isalnum(expanded[paramPos - 1]) || expanded[paramPos - 1] == '_')) {
                                            isParam = false;
                                        }
                                        if (paramPos + macro.params[i].length() < expanded.length() &&
                                            (std::isalnum(expanded[paramPos + macro.params[i].length()]) ||
                                                expanded[paramPos + macro.params[i].length()] == '_')) {
                                            isParam = false;
                                        }

                                        if (isParam) {
                                            expanded.replace(paramPos, macro.params[i].length(), args[i]);
                                            paramPos = expanded.find(macro.params[i], paramPos + args[i].length());
                                        }
                                        else {
                                            paramPos = expanded.find(macro.params[i], paramPos + 1);
                                        }
                                    }
                                }

                                result.replace(pos, parenEnd - pos + 1, expanded);
                                changed = true;
                                break;
                            }
                        }
                    }
                    else {
                        result.replace(pos, name.length(), macro.value);
                        changed = true;
                        break;
                    }
                }
                pos = result.find(name, pos + 1);
            }

            if (changed) break;
        }
    } while (changed);

    return result;
}

std::string Preprocessor::trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, last - first + 1);
}

double evaluateExpression(const std::string& expression) {
    std::stack<double> values;
    std::stack<char> ops;

    auto hasPrecedence = [](char op1, char op2) -> bool {
        if (op2 == '(' || op2 == ')')
            return false;
        if ((op1 == '*' || op1 == '/' || op1 == '%') && (op2 == '+' || op2 == '-'))
            return false;
        return true;
        };

    auto applyOperation = [](char op, double b, double a) -> double {
        switch (op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/':
            if (b == 0) throw std::runtime_error("Division by zero");
            return a / b;
        case '%': return fmod(a, b);
        case '^': return pow(a, b);
        default: throw std::runtime_error("Invalid operator");
        }
        };

    auto isOperator = [](char c) -> bool {
        return c == '+' || c == '-' || c == '*' || c == '/' || c == '%' || c == '^';
        };

    auto isUnaryMinus = [&](const std::string& expr, size_t i) -> bool {
        return expr[i] == '-' && (i == 0 || expr[i - 1] == '(' ||
            isOperator(expr[i - 1]) || expr[i - 1] == '=');
        };

    std::string expr = expression;

    expr.erase(std::remove(expr.begin(), expr.end(), ' '), expr.end());
    if (expr.empty()) return 0;

    for (size_t i = 0; i < expr.length(); i++) {
        if (expr[i] == ' ') continue;

        if (isdigit(expr[i]) || expr[i] == '.') {
            std::string numStr;
            while (i < expr.length() && (isdigit(expr[i]) || expr[i] == '.')) {
                numStr += expr[i++];
            }
            i--;

            double num;
            std::istringstream(numStr) >> num;
            values.push(num);
        }
        else if (isUnaryMinus(expr, i)) {
            values.push(0);
            ops.push('-');
        }
        else if (expr[i] == '(') {
            ops.push(expr[i]);
        }
        else if (expr[i] == ')') {
            while (!ops.empty() && ops.top() != '(') {
                double val2 = values.top(); values.pop();
                double val1 = values.top(); values.pop();
                char op = ops.top(); ops.pop();
                values.push(applyOperation(op, val2, val1));
            }
            if (ops.empty()) throw std::runtime_error("Mismatched parentheses");
            ops.pop();
        }
        else if (isOperator(expr[i])) {
            while (!ops.empty() && hasPrecedence(expr[i], ops.top())) {
                double val2 = values.top(); values.pop();
                double val1 = values.top(); values.pop();
                char op = ops.top(); ops.pop();
                values.push(applyOperation(op, val2, val1));
            }
            ops.push(expr[i]);
        }
        else if (i + 1 < expr.length() && expr.substr(i, 2) == "pi") {
            values.push(3.14159265358979323846);
            i += 1;
        }
        else if (i + 1 < expr.length() && expr.substr(i, 2) == "e(") {
            size_t start = i + 2;
            size_t end = expr.find(')', start);
            if (end == std::string::npos) throw std::runtime_error("Missing ')' for e()");

            std::string subExpr = expr.substr(start, end - start);
            double exponent = evaluateExpression(subExpr);
            values.push(exp(exponent));
            i = end;
        }
        else {
            throw std::runtime_error("Invalid character in expression");
        }
    }

    while (!ops.empty()) {
        if (ops.top() == '(') {
            throw std::runtime_error("Mismatched parentheses");
        }

        double val2 = values.top(); values.pop();
        double val1 = values.top(); values.pop();
        char op = ops.top(); ops.pop();
        values.push(applyOperation(op, val2, val1));
    }

    if (values.size() != 1) {
        throw std::runtime_error("Invalid expression");
    }

    return values.top();
}