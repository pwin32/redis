/*
 * HNSW (Hierarchical Navigable Small World) Implementation
 * Based on the paper by Yu. A. Malkov, D. A. Yashunin
 *
 * Copyright(C) 2024 Salvatore Sanfilippo. All Rights Reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>

#include "hnsw.h"

/* Get current time in milliseconds */
uint64_t ms_time(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + (tv.tv_usec / 1000);
}

/* Example usage in main() */
int w2v_single_thread(int quantization, uint64_t numele, int massdel, int recall) {
    /* Create index */
    HNSW *index = hnsw_new(300, quantization);
    float v[300];
    uint16_t wlen;

    FILE *fp = fopen("word2vec.bin","rb");
    if (fp == NULL) {
        perror("word2vec.bin file missing");
        exit(1);
    }
    unsigned char header[8];
    fread(header,8,1,fp); // Skip header

    uint64_t id = 0;
    uint64_t start_time = ms_time();
    char *word = NULL;
    hnswNode *search_node = NULL;

    while(id < numele) {
        if (fread(&wlen,2,1,fp) == 0) break;
        word = malloc(wlen+1);
        fread(word,wlen,1,fp);
        word[wlen] = 0;
        fread(v,300*sizeof(float),1,fp);

        // Plain API that acquires a write lock for the whole time.
        hnswNode *added = hnsw_insert(index, v, NULL, 0, id++, word, 200);

        if (!strcmp(word,"banana")) search_node = added;
        if (!(id % 10000)) printf("%llu added\n", (unsigned long long)id);
    }
    uint64_t elapsed = ms_time() - start_time;
    fclose(fp);

    printf("%llu words added (%llu words/sec), last word: %s\n",
        (unsigned long long)index->node_count,
        (unsigned long long)id*1000/elapsed, word);

    /* Search query */
    if (search_node == NULL) search_node = index->head;
    hnsw_get_node_vector(index,search_node,v);
    hnswNode *neighbors[10];
    float distances[10];

    int found, j;
    start_time = ms_time();
    for (j = 0; j < 20000; j++)
        found = hnsw_search(index, v, 10, neighbors, distances, 0, 0);
    elapsed = ms_time() - start_time;
    printf("%d searches performed (%llu searches/sec), nodes found: %d\n",
        j, (unsigned long long)j*1000/elapsed, found);

    if (found > 0) {
        printf("Found %d neighbors:\n", found);
        for (int i = 0; i < found; i++) {
            printf("Node ID: %llu, distance: %f, word: %s\n",
                   (unsigned long long)neighbors[i]->id,
                   distances[i], (char*)neighbors[i]->value);
        }
    }

    // Recall test (slow).
    if (recall) {
        hnsw_print_stats(index);
        hnsw_test_graph_recall(index,200,0);
    }

    uint64_t connected_nodes;
    int reciprocal_links;
    hnsw_validate_graph(index, &connected_nodes, &reciprocal_links);

    if (massdel) {
        int remove_perc = 95;
        printf("\nRemoving %d%% of nodes...\n", remove_perc);
        uint64_t initial_nodes = index->node_count;

        hnswNode *current = index->head;
        while (current && index->node_count > initial_nodes*(100-remove_perc)/100) {
            hnswNode *next = current->next;
            hnsw_delete_node(index,current,free);
            current = next;
            // In order to don't remove only contiguous nodes, from time
            // skip a node.
            if (current && !(random() % remove_perc)) current = current->next;
        }
        printf("%llu nodes left\n", (unsigned long long)index->node_count);

        // Test again.
        hnsw_validate_graph(index, &connected_nodes, &reciprocal_links);
        hnsw_test_graph_recall(index,200,0);
    }

    hnsw_free(index,free);
    return 0;
}

struct threadContext {
    pthread_mutex_t FileAccessMutex;
    uint64_t numele;
    _Atomic uint64_t SearchesDone;
    _Atomic uint64_t id;
    FILE *fp;
    HNSW *index;
    float *search_vector;
};

// Note that in practical terms inserting with many concurrent threads
// may be *slower* and not faster, because there is a lot of
// contention. So this is more a robustness test than anything else.
//
// The optimistic commit API goal is actually to exploit the ability to
// add faster when there are many concurrent reads.
void *threaded_insert(void *ctxptr) {
    struct threadContext *ctx = ctxptr;
    char *word;
    float v[300];
    uint16_t wlen;

    while(1) {
        pthread_mutex_lock(&ctx->FileAccessMutex);
        if (fread(&wlen,2,1,ctx->fp) == 0) break;
        pthread_mutex_unlock(&ctx->FileAccessMutex);
        word = malloc(wlen+1);
        fread(word,wlen,1,ctx->fp);
        word[wlen] = 0;
        fread(v,300*sizeof(float),1,ctx->fp);

        // Check-and-set API that performs the costly scan for similar
        // nodes concurrently with other read threads, and finally
        // applies the check if the graph wasn't modified.
        InsertContext *ic;
        uint64_t next_id = ctx->id++;
        ic = hnsw_prepare_insert(ctx->index, v, NULL, 0, next_id, word, 200);
        if (hnsw_try_commit_insert(ctx->index, ic) == NULL) {
            // This time try locking since the start.
            hnsw_insert(ctx->index, v, NULL, 0, next_id, word, 200);
        }

        if (next_id >= ctx->numele) break;
        if (!((next_id+1) % 10000))
            printf("%llu added\n", (unsigned long long)next_id+1);
    }
    return NULL;
}

void *threaded_search(void *ctxptr) {
    struct threadContext *ctx = ctxptr;

    /* Search query */
    hnswNode *neighbors[10];
    float distances[10];
    int found = 0;
    uint64_t last_id = 0;

    while(ctx->id < 1000000) {
        int slot = hnsw_acquire_read_slot(ctx->index);
        found = hnsw_search(ctx->index, ctx->search_vector, 10, neighbors, distances, slot, 0);
        hnsw_release_read_slot(ctx->index,slot);
        last_id = ++ctx->id;
    }

    if (found > 0 && last_id == 1000000) {
        printf("Found %d neighbors:\n", found);
        for (int i = 0; i < found; i++) {
            printf("Node ID: %llu, distance: %f, word: %s\n",
                   (unsigned long long)neighbors[i]->id,
                   distances[i], (char*)neighbors[i]->value);
        }
    }
    return NULL;
}

int w2v_multi_thread(int numthreads, int quantization, uint64_t numele) {
    /* Create index */
    struct threadContext ctx;

    ctx.index = hnsw_new(300,quantization);

    ctx.fp = fopen("word2vec.bin","rb");
    if (ctx.fp == NULL) {
        perror("word2vec.bin file missing");
        exit(1);
    }

    unsigned char header[8];
    fread(header,8,1,ctx.fp); // Skip header
    pthread_mutex_init(&ctx.FileAccessMutex,NULL);

    uint64_t start_time = ms_time();
    ctx.id = 0;
    ctx.numele = numele;
    pthread_t threads[numthreads];
    for (int j = 0; j < numthreads; j++)
        pthread_create(&threads[j], NULL, threaded_insert, &ctx);

    // Wait for all the threads to terminate adding items.
    for (int j = 0; j < numthreads; j++)
        pthread_join(threads[j],NULL);

    uint64_t elapsed = ms_time() - start_time;
    fclose(ctx.fp);

    // Obtain the last word.
    hnswNode *node = ctx.index->head;
    char *word = node->value;

    // We will search this last inserted word in the next test.
    // Let's save its embedding.
    ctx.search_vector = malloc(sizeof(float)*300);
    hnsw_get_node_vector(ctx.index,node,ctx.search_vector);

    printf("%llu words added (%llu words/sec), last word: %s\n",
        (unsigned long long)ctx.index->node_count,
        (unsigned long long)ctx.id*1000/elapsed, word);

    /* Search query */
    start_time = ms_time();
    ctx.id = 0; // We will use this atomic field to stop at N queries done.

    for (int j = 0; j < numthreads; j++)
        pthread_create(&threads[j], NULL, threaded_search, &ctx);

    // Wait for all the threads to terminate searching.
    for (int j = 0; j < numthreads; j++)
        pthread_join(threads[j],NULL);

    elapsed = ms_time() - start_time;
    printf("%llu searches performed (%llu searches/sec)\n",
        (unsigned long long)ctx.id,
        (unsigned long long)ctx.id*1000/elapsed);

    hnsw_print_stats(ctx.index);
    uint64_t connected_nodes;
    int reciprocal_links;
    hnsw_validate_graph(ctx.index, &connected_nodes, &reciprocal_links);
    printf("%llu connected nodes. Links all reciprocal: %d\n",
        (unsigned long long)connected_nodes, reciprocal_links);
    hnsw_free(ctx.index,free);
    return 0;
}

int main(int argc, char **argv) {
    int quantization = HNSW_QUANT_NONE;
    int numthreads = 0;
    uint64_t numele = 20000;

    /* This you can enable in single thread mode for testing: */
    int massdel = 0;    // If true, does the mass deletion test.
    int recall = 0;     // If true, does the recall test.

    for (int j = 1; j < argc; j++) {
        int moreargs = argc-j-1;

        if (!strcasecmp(argv[j],"--quant")) {
            quantization = HNSW_QUANT_Q8;
        } else if (!strcasecmp(argv[j],"--bin")) {
            quantization = HNSW_QUANT_BIN;
        } else if (!strcasecmp(argv[j],"--mass-del")) {
            massdel = 1;
        } else if (!strcasecmp(argv[j],"--recall")) {
            recall = 1;
        } else if (moreargs >= 1 && !strcasecmp(argv[j],"--threads")) {
            numthreads = atoi(argv[j+1]);
            j++;
        } else if (moreargs >= 1 && !strcasecmp(argv[j],"--numele")) {
            numele = strtoll(argv[j+1],NULL,0);
            j++;
            if (numele < 1) numele = 1;
        } else if (!strcasecmp(argv[j],"--help")) {
            printf("%s [--quant] [--bin] [--thread <count>] [--numele <count>] [--mass-del] [--recall]\n", argv[0]);
            exit(0);
        } else {
            printf("Unrecognized option: %s\n", argv[j]);
            exit(1);
        }
    }

    if (quantization == HNSW_QUANT_NONE) {
        printf("You can enable quantization with --quant\n");
    }

    if (numthreads > 0) {
        w2v_multi_thread(numthreads,quantization,numele);
    } else {
        printf("Single thread execution. Use --threads 4 for concurrent API\n");
        w2v_single_thread(quantization,numele,massdel,recall);
    }
}
