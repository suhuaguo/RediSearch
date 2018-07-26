#include "spell_check.h"
#include "util/arr.h"
#include "dictionary.h"
#include <stdbool.h>

/** Forward declaration **/
static bool SpellCheck_IsTermExistsInTrie(Trie *t, const char *term, size_t len);

static int RS_SuggestionCompare(const void *val1, const void *val2) {
  const RS_Suggestion *a = val1;
  const RS_Suggestion *b = val2;
  if (a->score > b->score) {
    return 1;
  }
  if (a->score < b->score) {
    return -1;
  }
  return 0;
}

static RS_Suggestion *RS_SuggestionCreate(char *suggestion, size_t len, double score) {
  RS_Suggestion *res = calloc(1, sizeof(RS_Suggestion));
  res->suggestion = suggestion;
  res->len = len;
  res->score = score;
  return res;
}

static void RS_SuggestionFree(RS_Suggestion *suggestion) {
  free(suggestion->suggestion);
  free(suggestion);
}

static RS_Suggestions *RS_SuggestionsCreate() {
#define SUGGESTIONS_ARRAY_INITIAL_SIZE 10
  RS_Suggestions *ret = calloc(1, sizeof(RS_Suggestions));
  ret->suggestionsTrie = NewTrie();
  ret->suggestions = array_new(RS_Suggestion *, SUGGESTIONS_ARRAY_INITIAL_SIZE);
  return ret;
}

static void RS_SuggestionsAdd(RS_Suggestions *s, char *term, size_t len, double score) {
  if (SpellCheck_IsTermExistsInTrie(s->suggestionsTrie, term, len)) {
    return;
  }

  Trie_InsertStringBuffer(s->suggestionsTrie, term, len, score, 1, NULL);
  s->suggestions = array_append(s->suggestions, RS_SuggestionCreate(term, len, score));
}

static void RS_SuggestionsFree(RS_Suggestions *s) {
  array_free_ex(s->suggestions, RS_SuggestionFree(*(RS_Suggestion **)ptr));
  TrieType_Free(s->suggestionsTrie);
  free(s);
}

/**
 * Return the score for the given suggestion (number between 0 to 1).
 * In case the suggestion should not be added return -1.
 */
static double SpellCheck_GetScore(SpellCheckCtx *scCtx, char *suggestion, size_t len,
                                  t_fieldMask fieldMask) {
  RedisModuleKey *keyp = NULL;
  InvertedIndex *invidx = Redis_OpenInvertedIndexEx(scCtx->sctx, suggestion, len, 0, &keyp);
  double retVal = 0;
  if (!invidx) {
    // can not find inverted index key, score is 0.
    goto end;
  }
  IndexReader *reader = NewTermIndexReader(invidx, NULL, fieldMask, NULL, 1);
  IndexIterator *iter = NewReadIterator(reader);
  RSIndexResult *r;
  if (iter->Read(iter->ctx, &r) != INDEXREAD_EOF) {
    // we have at least one result, the suggestion is relevant.
    if(scCtx->fullScoreInfo){
      retVal = invidx->numDocs;
    }else{
      retVal = invidx->numDocs / (double)(scCtx->sctx->spec->docs.size - 1);
    }
  } else {
    // fieldMask has filtered all docs, this suggestions should not be returned
    retVal = -1;
  }
  ReadIterator_Free(iter);

end:
  if (keyp) {
    RedisModule_CloseKey(keyp);
  }
  return retVal;
}

static bool SpellCheck_IsTermExistsInTrie(Trie *t, const char *term, size_t len) {
  rune *rstr = NULL;
  t_len slen = 0;
  float score = 0;
  int dist = 0;
  bool retVal = false;
  TrieIterator *it = Trie_Iterate(t, term, len, 0, 0);
  if (TrieIterator_Next(it, &rstr, &slen, NULL, &score, &dist)) {
    retVal = true;
  }
  DFAFilter_Free(it->ctx);
  free(it->ctx);
  TrieIterator_Free(it);
  return retVal;
}

static void SpellCheck_FindSuggestions(SpellCheckCtx *scCtx, Trie *t, const char *term, size_t len,
                                       t_fieldMask fieldMask, RS_Suggestions *s) {
  rune *rstr = NULL;
  t_len slen = 0;
  float score = 0;
  int dist = 0;
  size_t suggestionLen;

  TrieIterator *it = Trie_Iterate(t, term, len, (int)scCtx->distance, 0);
  while (TrieIterator_Next(it, &rstr, &slen, NULL, &score, &dist)) {
    char *res = runesToStr(rstr, slen, &suggestionLen);
    double score;
    if ((score = SpellCheck_GetScore(scCtx, res, suggestionLen, fieldMask)) != -1) {
      RS_SuggestionsAdd(s, res, suggestionLen, score);
    } else {
      free(res);
    }
  }
  DFAFilter_Free(it->ctx);
  free(it->ctx);
  TrieIterator_Free(it);
}

static bool SpellCheck_ReplyTermSuggestions(SpellCheckCtx *scCtx, char *term, size_t len,
                                            t_fieldMask fieldMask) {
#define NO_SUGGESTIONS_REPLY "no spelling corrections found"
#define TERM "TERM"

  // searching the term on the term trie, if its there we just return false
  // because there is no need to return suggestions on it.
  if (SpellCheck_IsTermExistsInTrie(scCtx->sctx->spec->terms, term, len)) {
    return false;
  }

  // searching the term on the exclude list, if its there we just return false
  // because there is no need to return suggestions on it.
  for (int i = 0; i < array_len(scCtx->excludeDict); ++i) {
    RedisModuleKey *k = NULL;
    Trie *t =
        SpellCheck_OpenDict(scCtx->sctx->redisCtx, scCtx->excludeDict[i], REDISMODULE_READ, &k);
    if (t == NULL) {
      continue;
    }
    if (SpellCheck_IsTermExistsInTrie(t, term, len)) {
      RedisModule_CloseKey(k);
      return false;
    }
    RedisModule_CloseKey(k);
  }

  RS_Suggestions *s = RS_SuggestionsCreate();

  RedisModule_ReplyWithArray(scCtx->sctx->redisCtx, 3);
  RedisModule_ReplyWithStringBuffer(scCtx->sctx->redisCtx, TERM, strlen(TERM));
  RedisModule_ReplyWithStringBuffer(scCtx->sctx->redisCtx, term, len);

  SpellCheck_FindSuggestions(scCtx, scCtx->sctx->spec->terms, term, len, fieldMask, s);

  // sorting results by score
  qsort(s->suggestions, array_len(s->suggestions), sizeof(RS_Suggestion *), RS_SuggestionCompare);

  // searching the term on the include list for more suggestions.
  for (int i = 0; i < array_len(scCtx->includeDict); ++i) {
    RedisModuleKey *k = NULL;
    Trie *t =
        SpellCheck_OpenDict(scCtx->sctx->redisCtx, scCtx->includeDict[i], REDISMODULE_READ, &k);
    if (t == NULL) {
      continue;
    }
    SpellCheck_FindSuggestions(scCtx, t, term, len, fieldMask, s);
    RedisModule_CloseKey(k);
  }

  if (array_len(s->suggestions) == 0) {
    RedisModule_ReplyWithStringBuffer(scCtx->sctx->redisCtx, NO_SUGGESTIONS_REPLY,
                                      strlen(NO_SUGGESTIONS_REPLY));
  } else {
    RedisModule_ReplyWithArray(scCtx->sctx->redisCtx, array_len(s->suggestions));
    for (int i = 0; i < array_len(s->suggestions); ++i) {
      RedisModule_ReplyWithArray(scCtx->sctx->redisCtx, 2);
      RedisModule_ReplyWithDouble(scCtx->sctx->redisCtx, s->suggestions[i]->score);
      RedisModule_ReplyWithStringBuffer(scCtx->sctx->redisCtx, s->suggestions[i]->suggestion,
                                        s->suggestions[i]->len);
    }
  }

  RS_SuggestionsFree(s);

  return true;
}

static bool SpellCheck_CheckDictExistence(SpellCheckCtx *scCtx, char *dict) {
#define BUFF_SIZE 1000
  RedisModuleKey *k = NULL;
  Trie *t = SpellCheck_OpenDict(scCtx->sctx->redisCtx, dict, REDISMODULE_READ, &k);
  if (t == NULL) {
    char buff[BUFF_SIZE];
    snprintf(buff, BUFF_SIZE, "the given dict are not exists: %s", dict);
    RedisModule_ReplyWithError(scCtx->sctx->redisCtx, buff);
    return false;
  }
  RedisModule_CloseKey(k);
  return true;
}

static bool SpellCheck_CheckTermDictsExistance(SpellCheckCtx *scCtx) {
  for (int i = 0; i < array_len(scCtx->includeDict); ++i) {
    if (!SpellCheck_CheckDictExistence(scCtx, scCtx->includeDict[i])) {
      return false;
    }
  }

  for (int i = 0; i < array_len(scCtx->excludeDict); ++i) {
    if (!SpellCheck_CheckDictExistence(scCtx, scCtx->excludeDict[i])) {
      return false;
    }
  }

  return true;
}

void SpellCheck_Reply(SpellCheckCtx *scCtx, QueryParseCtx *q) {
#define NODES_INITIAL_SIZE 5
  if (!SpellCheck_CheckTermDictsExistance(scCtx)) {
    return;
  }
  size_t results = 0;

  QueryNode **nodes = array_new(QueryNode *, NODES_INITIAL_SIZE);
  nodes = array_append(nodes, q->root);
  QueryNode *currNode = NULL;

  RedisModule_ReplyWithArray(scCtx->sctx->redisCtx, REDISMODULE_POSTPONED_ARRAY_LEN);

  if(scCtx->fullScoreInfo){
    // sending the total number of docs for the ability to calculate score on cluster
    RedisModule_ReplyWithLongLong(scCtx->sctx->redisCtx, scCtx->sctx->spec->docs.size - 1);
  }

  while (array_len(nodes) > 0) {
    currNode = array_pop(nodes);

    switch (currNode->type) {
      case QN_PHRASE:
        for (int i = 0; i < currNode->pn.numChildren; i++) {
          nodes = array_append(nodes, currNode->pn.children[i]);
        }
        break;
      case QN_TOKEN:
        if (SpellCheck_ReplyTermSuggestions(scCtx, currNode->tn.str, currNode->tn.len,
                                            currNode->opts.fieldMask)) {
          ++results;
        }
        break;

      case QN_NOT:
        nodes = array_append(nodes, currNode->not.child);
        break;

      case QN_OPTIONAL:
        nodes = array_append(nodes, currNode->opt.child);
        break;

      case QN_UNION:
        for (int i = 0; i < currNode->un.numChildren; i++) {
          nodes = array_append(nodes, currNode->un.children[i]);
        }
        break;

      case QN_TAG:
        // todo: do we need to do enything here?
        for (int i = 0; i < currNode->tag.numChildren; i++) {
          nodes = array_append(nodes, currNode->tag.children[i]);
        }
        break;

      case QN_PREFX:
      case QN_NUMERIC:
      case QN_GEO:
      case QN_IDS:
      case QN_WILDCARD:
      case QN_FUZZY:
        break;
    }
  }

  array_free(nodes);

  RedisModule_ReplySetArrayLength(scCtx->sctx->redisCtx, results + (scCtx->fullScoreInfo ? 1 : 0));
}
