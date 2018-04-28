// Copyright 2011 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef NINJA_EVAL_ENV_H_
#define NINJA_EVAL_ENV_H_

#include <map>
#include <string>
#include <vector>

#include <string_view>

struct Rule;

/// An interface for a scope for variable (e.g. "$foo") lookups.
struct Env {
  virtual ~Env() {}
  virtual std::string LookupVariable(const std::string& var) = 0;
};

/// A tokenized std::string that contains variable references.
/// Can be evaluated relative to an Env.
struct EvalString {
  std::string Evaluate(Env* env) const;

  void Clear() { parsed_.clear(); }
  bool empty() const { return parsed_.empty(); }

  void AddText(std::string_view text);
  void AddSpecial(std::string_view text);

  /// Construct a human-readable representation of the parsed state,
  /// for use in tests.
  std::string Serialize() const;

private:
  enum TokenType { RAW, SPECIAL };
  typedef std::vector<std::pair<std::string, TokenType> > TokenList;
  TokenList parsed_;
};

/// An invokable build command and associated metadata (description, etc.).
struct Rule {
  explicit Rule(const std::string& name) : name_(name) {}

  const std::string& name() const { return name_; }

  void AddBinding(const std::string& key, const EvalString& val);

  static bool IsReservedBinding(const std::string& var);

  const EvalString* GetBinding(const std::string& key) const;

 private:
  // Allow the parsers to reach into this object and fill out its fields.
  friend struct ManifestParser;

  std::string name_;
  typedef std::map<std::string, EvalString> Bindings;
  Bindings bindings_;
};

/// An Env which contains a mapping of variables to values
/// as well as a pointer to a parent scope.
struct BindingEnv : public Env {
  BindingEnv() : parent_(nullptr) {}
  explicit BindingEnv(BindingEnv* parent) : parent_(parent) {}

  virtual ~BindingEnv() {}
  virtual std::string LookupVariable(const std::string& var);

  void AddRule(const Rule* rule);
  const Rule* LookupRule(const std::string& rule_name);
  const Rule* LookupRuleCurrentScope(const std::string& rule_name);
  const std::map<std::string, const Rule*>& GetRules() const;

  void AddBinding(const std::string& key, const std::string& val);

  /// This is tricky.  Edges want lookup scope to go in this order:
  /// 1) value set on edge itself (edge_->env_)
  /// 2) value set on rule, with expansion in the edge's scope
  /// 3) value set on enclosing scope of edge (edge_->env_->parent_)
  /// This function takes as parameters the necessary info to do (2).
  std::string LookupWithFallback(const std::string& var, const EvalString* eval, Env* env);

private:
  std::map<std::string, std::string> bindings_;
  std::map<std::string, const Rule*> rules_;
  BindingEnv* parent_;
};

#endif  // NINJA_EVAL_ENV_H_
