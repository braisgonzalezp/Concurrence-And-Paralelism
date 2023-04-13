#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <threads.h>
#include "options.h"

#define DELAY_SCALE 1000


struct array {
    int size;
    int *arr;
    mtx_t* mutex;        // mutex
};

struct args {
    mtx_t            *mutex;       // mutex for the iterations
    int              thread_num;  // application defined thread #
    int              delay;       // delay between operations
    int	             *iterations; // number of operations
    struct array     *array;
};

struct thread_info {
    pthread_t    id;    // id returned by pthread_create()
    struct args *args;  // pointer to the arguments
};



int down(int* iterations, mtx_t* mutex){//Disminuir 1 iteración
    int aux;
    mtx_lock(mutex);

    if(*iterations){
        aux = (*iterations)--;
    } else{
        aux = 0;
    }

    mtx_unlock(mutex);
    return aux;
}

int up(int* iterations, mtx_t* mutex){//Aumentar 1 iteracion
    int aux;
    mtx_lock(mutex);
    aux = (*iterations)++;
    mtx_unlock(mutex);
    return aux;
}


void apply_delay(int delay) {
    for(int i = 0; i < delay * DELAY_SCALE; i++); // waste time
}




int increment(void *ptr) //Función incrementar valor
{
    struct args *args = ptr;
    int pos, val;

    while(down(args->iterations,args->mutex)) {

        pos = rand() % args->array->size;//posicion random

        printf("%d increasing position %d\n", args->thread_num, pos);

        mtx_lock(&args->array->mutex[pos]);
        //lioso aposta para que no funcione
        val = args->array->arr[pos];
        if(args->delay) usleep(args->delay);

        val ++;
        if(args->delay) usleep(args->delay);

        args->array->arr[pos] = val;
        if(args->delay) usleep(args->delay);
        mtx_unlock(&args->array->mutex[pos]);
    }

    return 0;
}


int changePos(void *ptr) { //Funcion cambiar valores a posiciones
    struct args *args = ptr;
    int pos1,pos2;

    while (down(args->iterations, args->mutex)) {

        pos1 = rand() % args->array->size;
        pos2 = rand() % args->array->size;

        if(pos1 == pos2){
            up(args->iterations, args->mutex);
            continue;
        }

        while (1) {
            mtx_lock(&args->array->mutex[pos1]);
            if(mtx_trylock(&args->array->mutex[pos2])){
                mtx_unlock(&args->array->mutex[pos1]);
                continue;
            }

            printf("Thread %d: Decreasing 1 in position %d and increasing 2 in position %d\n", args->thread_num, pos1,
                   pos2);
            args->array->arr[pos1] -= 1;
            if (args->delay) usleep(args->delay);

            args->array->arr[pos2] += 2;
            if (args->delay) usleep(args->delay);


            mtx_unlock(&args->array->mutex[pos1]);
            mtx_unlock(&args->array->mutex[pos2]);
            break;
        }
    }
    return 0;
}



struct thread_info *startThreads(struct options opt, struct array *arr,int (tipo) (void *)) //CREO EL NUMERO DE THREADS INDICADOS
{
    int i, *iterations;
    struct thread_info *threads;
    mtx_t *mutex;

    printf("\nCreating %d threads\n", opt.num_threads);
    threads = malloc(sizeof(struct thread_info) * opt.num_threads);

    if (threads == NULL) {
        printf("Not enough memory\n");
        exit(1);
    }

    mutex = malloc(sizeof (pthread_mutex_t));
    iterations= malloc(sizeof (int));
    mtx_init(mutex,1);
    *iterations = opt.iterations;

    // Create num_thread threads running swap()
    for (i = 0; i < opt.num_threads; i++) {
        threads[i].args = malloc(sizeof(struct args));

        threads[i].args -> thread_num = i;
        threads[i].args -> array       = arr;
        threads[i].args -> delay      = opt.delay;
        threads[i].args -> iterations = iterations;
        threads[i].args->mutex          = mutex;

        if (0 != thrd_create(&threads[i].id, tipo, threads[i].args)) {
            printf("Could not create thread #%d", i);
            exit(1);
        }
    }
    return threads;
}




void print_array(struct array arr) {
    int total = 0;

    for(int i = 0; i < arr.size; i++) {
        total += arr.arr[i];
        printf("%d ", arr.arr[i]);
    }

    printf("\nTotal: %d\n", total);
}


void wait(struct options opt, struct array *arr, struct thread_info *threads) {
    // Wait for the threads to finish
    for (int i = 0; i < opt.num_threads; i++)
        thrd_join(threads[i].id, NULL);


    // Solamente liberamos la memoria del thread 0 en estos tres casos porque la posicion de memoria
    // reservada para esos campos es la misma en todos los threads.
    free(threads[0].args->iterations);
    mtx_destroy(threads[0].args->mutex);
    free(threads[0].args->mutex);

    for (int i = 0; i < opt.num_threads; i++)
        free(threads[i].args);

    free(threads);

}


void wait2(struct options opt, struct array *arr, struct thread_info *threads) {
    // Wait for the threads to finish
    for (int i = 0; i < opt.num_threads; i++)
        thrd_join(threads[i].id, NULL);


    // Solamente liberamos la memoria del thread 0 en estos tres casos porque la posicion de memoria
    // reservada para esos campos es la misma en todos los threads.
    free(threads[0].args->iterations);
    mtx_destroy(threads[0].args->mutex);
    free(threads[0].args->mutex);

    for (int i = 0; i < opt.num_threads; i++)
        free(threads[i].args);

    free(threads);

    for (int i = 0; i < arr->size; ++i) {
        mtx_destroy(&arr->mutex[i]);
    }
    free(arr->mutex);
}

void initArray(struct array *arr, int size) {
    arr->size = size;
    arr->arr = malloc(arr->size * sizeof(int));
    arr->mutex = malloc((arr->size) * sizeof(pthread_mutex_t));

    for(int i=0; i < arr->size; i++) {
        arr->arr[i] = 0;
        mtx_init(&arr->mutex[i], 1);
    }
}


int main (int argc, char **argv)
{
    struct options       opt;
    struct array         arr;
    struct thread_info *thrs;
    struct thread_info *pos_thrs;

    srand(time(NULL));

    // Default values for the options
    opt.num_threads  = 5;
    opt.size         = 10;
    opt.iterations   = 100;
    opt.delay        = 1000;

    read_options(argc, argv, &opt);

    arr.size = opt.size;

    initArray(&arr,opt.size);

    thrs = startThreads(opt, &arr,increment);//CREACION Y EJECUCION THREADS

    wait(opt, &arr, thrs);//ACABAR THREADS Y LIBERAR MEMORIA

    print_array(arr);

    pos_thrs = startThreads(opt,&arr,changePos);

    wait2(opt,&arr,pos_thrs);

    print_array(arr);

    free(arr.arr);
    return 0;
}
