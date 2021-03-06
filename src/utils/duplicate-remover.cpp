/* duplicate-remover:
 *
 * Copyright (C) 2013-2018 University of Southern California and
 *                         Andrew D. Smith
 *
 * Authors: Andrew D. Smith, Ben Decato, Song Qiang
 *
 * This program is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include "OptionParser.hpp"
#include "smithlab_utils.hpp"
#include "smithlab_os.hpp"
#include "GenomicRegion.hpp"
#include "MappedRead.hpp"
#include "bsutils.hpp"
#include "zlib_wrapper.hpp"

#include <string>
#include <vector>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <stdexcept>

// #include <sys/types.h>
// #include <unistd.h>

using std::string;
using std::vector;
using std::cin;
using std::cout;
using std::cerr;
using std::endl;
using std::ifstream;
using std::ofstream;
using std::unordered_map;
using std::unordered_set;
using std::runtime_error;


static bool
precedes(const MappedRead &a, const MappedRead &b) {
  return (a.r.get_chrom() < b.r.get_chrom() ||
          (a.r.get_chrom() == b.r.get_chrom() &&
           (a.r.get_start() < b.r.get_start() ||
            (a.r.get_start() == b.r.get_start() &&
             (a.r.get_end() < b.r.get_end() ||
              (a.r.get_end() == b.r.get_end() &&
               (a.r.get_strand() < b.r.get_strand())))))));
}


static bool
equivalent(const MappedRead &a, const MappedRead &b) {
  return a.r.same_chrom(b.r) &&
    a.r.get_start() == b.r.get_start() &&
    a.r.get_end() == b.r.get_end() &&
    a.r.get_strand() == b.r.get_strand();
}


static void
get_cpgs(const vector<MappedRead> &mr, vector<size_t> &cpg_pos) {
  const size_t lim = mr.front().seq.length();
  for (size_t i = 1; i < lim; ++i) {
    size_t j = 0;
    while (j < mr.size() && !is_cpg(mr[j].seq, i - 1)) ++j;
    if (j < mr.size())
      cpg_pos.push_back(i - 1);
  }
}


static void
get_cytosines(const vector<MappedRead> &mr, vector<size_t> &c_pos) {
  const size_t lim = mr.front().seq.length();
  for (size_t i = 0; i < lim; ++i) {
    size_t j = 0;
    while (j < mr.size() && !is_cytosine(mr[j].seq[i])) ++j;
    if (j < mr.size())
      c_pos.push_back(i);
  }
}


static void
get_meth_patterns(const bool ALL_C,
                  vector<MappedRead> &mr, vector<size_t> &hist) {

  vector<size_t> sites;
  if (ALL_C)
    get_cytosines(mr, sites);
  else get_cpgs(mr, sites);

  unordered_map<string, vector<size_t> > patterns;
  for (size_t i = 0; i < mr.size(); ++i) {
    string s;
    for (size_t j = 0; j < sites.size(); ++j)
      s += (is_cytosine(mr[i].seq[sites[j]]) ? '1' : '0');
    patterns[s].push_back(i);
  }

  unordered_set<size_t> keepers;
  for (auto i(patterns.begin()); i != end(patterns); ++i) {
    const size_t n_dups = i->second.size();
    keepers.insert(i->second[rand() % n_dups]);
    if (hist.size() <= n_dups)
      hist.resize(n_dups + 1);
    hist[n_dups]++;
  }

  size_t j = 0;
  for (size_t i = 0; i < mr.size(); ++i)
    if (keepers.find(i) != end(keepers)) {
      mr[j] = mr[i];
      ++j;
    }
  mr.erase(begin(mr) + j, end(mr));
}


template <class T>
static void
duplicate_remover(const bool VERBOSE,
                  const bool USE_SEQUENCE,
                  const bool ALL_C,
                  const bool DISABLE_SORT_TEST,
                  const string &infile,
                  const string &statfile,
                  const string &histfile,
                  T &out) {

  // histogram is tabulated whether or not user requests it
  vector<size_t> hist;

  igzfstream in(infile);

  MappedRead mr;
  if (!(in >> mr))
    throw runtime_error("error reading file: " + infile);

  size_t reads_in = 1;
  size_t reads_out = 0;
  size_t good_bases_in = mr.seq.length();
  size_t good_bases_out = 0;
  size_t reads_with_duplicates = 0;

  vector<MappedRead> buffer(1, mr);
  while (in >> mr) {
    ++reads_in;
    good_bases_in += mr.seq.length();
    if (!DISABLE_SORT_TEST && precedes(mr, buffer.front()))
      throw runtime_error("input not properly sorted:\n" +
                          toa(buffer.front()) + "\n" + toa(mr));
    if (!equivalent(buffer.front(), mr)) {
      if (USE_SEQUENCE) {
        const size_t orig_buffer_size = buffer.size();
        get_meth_patterns(ALL_C, buffer, hist); // select within buffer
        for (auto && r : buffer)
          out << r << "\n";
        reads_out += buffer.size();
        good_bases_out += buffer.size()*buffer[0].seq.length();
        reads_with_duplicates += (buffer.size() < orig_buffer_size);
      }
      else {
        const size_t selected = rand() % buffer.size();
        out << buffer[selected] << "\n";
        if (hist.size() <= buffer.size())
          hist.resize(buffer.size() + 1);
        hist[buffer.size()]++;
        good_bases_out += buffer[selected].seq.length();
        ++reads_out;
        reads_with_duplicates += (buffer.size() > 1);
      }
      buffer.clear();
    }
    buffer.push_back(mr);
  }

  if (USE_SEQUENCE) {
    const size_t orig_buffer_size = buffer.size();
    get_meth_patterns(ALL_C, buffer, hist);
    for (auto && r : buffer)
      out << r << "\n";
    reads_out += buffer.size();
    good_bases_out += buffer.size()*buffer[0].seq.length();
    reads_with_duplicates += (buffer.size() < orig_buffer_size);
  }
  else {
    const size_t selected = rand() % buffer.size();
    out << buffer[selected] << "\n";
    if (hist.size() <= buffer.size())
      hist.resize(buffer.size() + 1);
    hist[buffer.size()]++;
    good_bases_out += buffer[selected].seq.length();
    ++reads_out;
    reads_with_duplicates += (buffer.size() > 1);
  }

  if (!statfile.empty()) {

    const size_t reads_removed = reads_in - reads_out;
    const double non_dup_fraction =
      static_cast<double>(reads_out - reads_with_duplicates)/reads_in;
    const double duplication_rate =
      static_cast<double>(reads_removed + reads_with_duplicates)/
      reads_with_duplicates;

    ofstream out_stat(statfile);
    out_stat << "total_reads: " << reads_in << endl
             << "total_bases: " << good_bases_in << endl
             << "unique_reads: " << reads_out << endl
             << "unique_read_bases: " << good_bases_out << endl
             << "non_duplicate_fraction: " << non_dup_fraction << endl
             << "duplicate_reads: " << reads_with_duplicates << endl
             << "reads_removed: " << reads_removed << endl
             << "duplication_rate: "
             << duplication_rate << endl;
  }
  if (!histfile.empty()) {
    ofstream out_hist(histfile);
    for (size_t i = 0; i < hist.size(); ++i)
      if (hist[i] > 0)
        out_hist << i << '\t' << hist[i] << '\n';
  }
}

int main(int argc, const char **argv) {

  try {
    bool VERBOSE = false;
    bool USE_SEQUENCE = false;
    bool ALL_C = false;
    bool DISABLE_SORT_TEST = false;
    bool INPUT_FROM_STDIN = false;

    size_t the_seed = 408;
    string outfile;
    string statfile;
    string histfile;

    /****************** COMMAND LINE OPTIONS ********************/
    OptionParser opt_parse(strip_path(argv[0]), "program to remove "
                           "duplicate reads from sorted mapped reads",
                           "<mapped-reads>");
    opt_parse.add_opt("output", 'o', "output file for unique reads",
                      false, outfile);
    opt_parse.add_opt("stdin", '\0', "take input from stdin",
                      false, INPUT_FROM_STDIN);
    opt_parse.add_opt("stats", 'S', "statistics output file", false, statfile);
    opt_parse.add_opt("hist", '\0', "histogram output file for library"
                      " complexity analysis", false, histfile);
    opt_parse.add_opt("seq", 's', "use sequence info", false, USE_SEQUENCE);
    opt_parse.add_opt("all-cytosines", 'A', "use all cytosines (default: CpG)",
                      false, ALL_C);
    opt_parse.add_opt("disable", 'D', "disable sort test",
                      false, DISABLE_SORT_TEST);
    opt_parse.add_opt("seed", 's', "specify random seed", false, the_seed);
    opt_parse.add_opt("verbose", 'v', "print more run info", false, VERBOSE);
    opt_parse.set_show_defaults();

    vector<string> leftover_args;
    opt_parse.parse(argc, argv, leftover_args);
    if (opt_parse.help_requested()) {
      cerr << opt_parse.help_message() << endl
           << opt_parse.about_message() << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.about_requested()) {
      cerr << opt_parse.about_message() << endl;
      return EXIT_SUCCESS;
    }
    if (opt_parse.option_missing()) {
      cerr << opt_parse.option_missing_message() << endl;
      return EXIT_SUCCESS;
    }
    if (!INPUT_FROM_STDIN && leftover_args.size() != 1) {
      cerr << opt_parse.help_message() << endl
           << opt_parse.about_message() << endl;
      return EXIT_SUCCESS;
    }
    string infile;
    if (!leftover_args.empty())
      infile = leftover_args.front();
    /****************** END COMMAND LINE OPTIONS *****************/

    srand(the_seed);

    if (outfile.empty() || !has_gz_ext(outfile)) {
      ofstream of;
      if (!outfile.empty()) of.open(outfile);
      std::ostream out(outfile.empty() ? cout.rdbuf() : of.rdbuf());
      if (!outfile.empty() && !out)
        throw runtime_error("failed to open output file: " + outfile);
      duplicate_remover(VERBOSE, USE_SEQUENCE, ALL_C, DISABLE_SORT_TEST,
                        infile, statfile, histfile, out);
    }
    else {
      ogzfstream out(outfile);
      duplicate_remover(VERBOSE, USE_SEQUENCE, ALL_C, DISABLE_SORT_TEST,
                        infile, statfile, histfile, out);
    }
  }
  catch (const runtime_error &e) {
    cerr << e.what() << endl;
    return EXIT_FAILURE;
  }
  catch (std::bad_alloc &ba) {
    cerr << "ERROR: could not allocate memory" << endl;
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
