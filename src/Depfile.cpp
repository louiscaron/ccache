// Copyright (C) 2020-2022 Joel Rosdahl and other contributors
//
// See doc/AUTHORS.adoc for a complete list of contributors.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 51
// Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "Depfile.hpp"

#include "Context.hpp"
#include "Hash.hpp"
#include "Logging.hpp"
#include "assertions.hpp"

#include <core/exceptions.hpp>
#include <util/file.hpp>
#include <util/path.hpp>

#include <algorithm>

static inline bool
is_blank(const std::string& s)
{
  return std::all_of(s.begin(), s.end(), [](char c) { return isspace(c); });
}

namespace Depfile {

std::string
escape_filename(std::string_view filename)
{
  std::string result;
  result.reserve(filename.size());
  for (const char c : filename) {
    switch (c) {
    case '\\':
    case '#':
    case ':':
    case ' ':
    case '\t':
      result.push_back('\\');
      break;
    case '$':
      result.push_back('$');
      break;
    }
    result.push_back(c);
  }
  return result;
}

std::optional<std::string>
rewrite_source_paths(const Context& ctx, std::string_view file_content)
{
  ASSERT(!ctx.config.base_dir().empty());

  // Fast path for the common case:
  if (file_content.find(ctx.config.base_dir()) == std::string::npos) {
    return std::nullopt;
  }

  std::string adjusted_file_content;
  adjusted_file_content.reserve(file_content.size());

  bool content_rewritten = false;
  bool seen_target_token = false;

  using util::Tokenizer;
  for (const auto line : Tokenizer(file_content,
                                   "\n",
                                   Tokenizer::Mode::include_empty,
                                   Tokenizer::IncludeDelimiter::yes)) {
    const auto tokens = Util::split_into_views(line, " \t");
    for (size_t i = 0; i < tokens.size(); ++i) {
      DEBUG_ASSERT(!line.empty()); // line.empty() -> no tokens
      DEBUG_ASSERT(!tokens[i].empty());

      if (i > 0 || line[0] == ' ' || line[0] == '\t') {
        adjusted_file_content.push_back(' ');
      }

      const auto& token = tokens[i];
      bool token_rewritten = false;
      if (seen_target_token && util::is_absolute_path(token)) {
        const auto new_path = Util::make_relative_path(ctx, token);
        if (new_path != token) {
          adjusted_file_content.append(new_path);
          token_rewritten = true;
        }
      }
      if (token_rewritten) {
        content_rewritten = true;
      } else {
        adjusted_file_content.append(token.begin(), token.end());
      }

      if (tokens[i].back() == ':') {
        seen_target_token = true;
      }
    }
  }

  if (content_rewritten) {
    return adjusted_file_content;
  } else {
    return std::nullopt;
  }
}

// Replace absolute paths with relative paths in the provided dependency file.
void
make_paths_relative_in_output_dep(const Context& ctx)
{
  if (ctx.config.base_dir().empty()) {
    LOG_RAW("Base dir not set, skip using relative paths");
    return; // nothing to do
  }

  const std::string& output_dep = ctx.args_info.output_dep;
  const auto file_content = util::read_file<std::string>(output_dep);
  if (!file_content) {
    LOG("Cannot open dependency file {}: {}", output_dep, file_content.error());
    return;
  }
  const auto new_content = rewrite_source_paths(ctx, *file_content);
  if (new_content) {
    util::write_file(output_dep, *new_content);
  } else {
    LOG("No paths in dependency file {} made relative", output_dep);
  }
}

std::vector<std::string>
tokenize(std::string_view file_content)
{
  // A dependency file uses Makefile syntax. This is not perfect parser but
  // should be enough for parsing a regular dependency file.
  // enhancement:
  // - space between target and colon
  // - no space between colon and first pre-requisite
  // the later is pretty complex because of the windows paths which are
  // identical to a target-colon-prerequisite without spaces (e.g. cat:/meow vs.
  // c:/meow) here are the tests on windows gnu make 4.3 how it handles this:
  //  + cat:/meow -> sees "cat" and "/meow"
  //  + cat:\meow -> sees "cat" and "\meow"
  //  + cat:\ meow -> sees "cat" and " meow"
  //  + cat:c:/meow -> sees "cat" and "c:/meow"
  //  + cat:c:\meow -> sees "cat" and "c:\meow"
  //  + cat:c: -> target pattern contains no '%'.  Stop.
  //  + cat:c:\ -> target pattern contains no '%'.  Stop.
  //  + cat:c:/ -> sees "cat" and "c:/"
  //  + cat:c:meow -> target pattern contains no '%'.  Stop.
  //  + c:c:/meow -> sees "c" and "c:/meow"
  //  + c:c:\meow -> sees "c" and "c:\meow"
  //  + c:z:\meow -> sees "c" and "z:\meow"
  //  + c:cd:\meow -> target pattern contains no '%'.  Stop.

  // the logic for a windows path is:
  //  - if there is a colon, if the previous token is 1 char long
  //    and that the following char is a slash (fw or bw), then it is
  //    a windows path

  std::vector<std::string> result;
  const size_t length = file_content.size();
  std::string token;
  size_t p = 0;

  while (p < length) {
    char c = file_content[p];

    if (c == ':') {
      if (p + 1 < length && !is_blank(token) && token.length() == 1) {
        const char next = file_content[p + 1];
        if (next == '/' || next == '\\') {
          // only in this case, this is not a separator and colon is
          // added to token
          token.push_back(c);
          ++p;
          continue;
        }
      }
    }
    // Each token is separated by whitespace or a colon.
    if (isspace(c) || c == ':') {
      // chomp all spaces before next char
      while (p < length && isspace(file_content[p])) {
        ++p;
      }
      if (!is_blank(token)) {
        // if there were spaces between a token and the : sign, the :
        // must be added to the same token to make sure it is seen as
        // a target and not as a dependency (ccache requirement)
        if (p < length) {
          const char next = file_content[p];
          if (next == ':') {
            token.push_back(next);
            ++p;
            // chomp all spaces before next char
            while (p < length && isspace(file_content[p])) {
              ++p;
            }
          }
        }
        result.push_back(token);
      }
      token.clear();
      continue;
    }

    switch (c) {
    case '\\':
      if (p + 1 < length) {
        const char next = file_content[p + 1];
        switch (next) {
        // A backspace followed by any of the below characters leaves the
        // character as is.
        case '\\':
        case '#':
        case ':':
        case ' ':
        case '\t':
          c = next;
          ++p;
          break;
        // Backslash followed by newline is interpreted like a space, so simply
        // discard the backslash.
        case '\n':
          ++p;
          continue;
        }
      }
      break;
    case '$':
      if (p + 1 < length) {
        const char next = file_content[p + 1];
        if (next == '$') {
          // A dollar sign preceded by a dollar sign escapes the dollar sign.
          c = next;
          ++p;
        }
      }
      break;
    // this is specific to TASKING compiler: filenames are quoted (not
    // supported by gnu make)
    case '"':
      // quotes should take everything until next quotes
      // skip the first quote
      ++p;
      while (p < length) {
        const char next = file_content[p];
        if (next == '"') {
          // skip the last quote
          ++p;
          break;
        } else {
          token.push_back(next);
          ++p;
        }
      }
      continue;
    }

    token.push_back(c);
    ++p;
  }

  if (!is_blank(token)) {
    result.push_back(token);
  }

  return result;
}

} // namespace Depfile
