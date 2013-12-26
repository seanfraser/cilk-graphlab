#include <assert.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "bag.cpp"
#include "Graph.cpp"
#include "engine.cpp"

#include <opencv/cv.h>
#include <opencv/highgui.h>
using namespace cv;
using namespace std;
void pagerank_update(int vid,
                     void* scheduler_void);

#include "scheduler.cpp"

// Parameters for the pagerank demo application
double termination_bound = 1e-10;
double random_reset_prob = 0.15;   // PageRank random reset probability

// Simple [0,1] RNG
double tfkRand(double fMin, double fMax)
{
    double f = (double)rand() / RAND_MAX;
    return fMin + f * (fMax - fMin);
}

// Simple timer for benchmarks
double tfk_get_time()
{
    struct timeval t;
    struct timezone tzp;
    gettimeofday(&t, &tzp);
    return t.tv_sec + t.tv_usec*1e-6;
}

// Programmer defined vertex and edge structures.
struct vdata{
  double value;
  double new_value;
  double self_weight; // GraphLab does not support edges from vertex to itself, so
  // we save weight of vertex's self-edge in the vertex data
  vdata(double value = 1) : value(value), self_weight(0) { }
};

struct edata {
  double weight;
  double old_source_value;
  double new_source_value;
  edata(double weight = 1) :
    weight(weight), old_source_value(0) { } 
};

// The scheduler and graph objects.
Scheduler* scheduler;
Graph<vdata, edata>* graph;

/**
 * Load a graph file specified in the format:
 *
 *   source_id <tab> target_id <tab> weight
 *   source_id <tab> target_id <tab> weight
 *   source_id <tab> target_id <tab> weight
 *               ....
 *
 * The file should not contain repeated edges.
 */
bool load_graph_from_file(const std::string& filename) {
  std::ifstream fin(filename.c_str());
  if(!fin.good()) return false;
  // Loop through file reading each line
  while(fin.good() && !fin.eof()) {
    size_t source = 0;
    size_t target = 0;
    float weight = -1;
    fin >> source;
    if(!fin.good()) break;
    //  fin.ignore(1); // skip comma
    fin >> target;
    assert(fin.good());
    //  fin.ignore(1); // skip comma
    fin >> weight;
    assert(fin.good());
    // Ensure that the number of vertices is correct
    if(source >= graph->num_vertices() ||
       target >= graph->num_vertices())
      graph->resize(std::max(source, target) + 1);
    if(source != target) {
      // Add the edge
      edata e(weight);
      graph->addEdge(source, target, e);
    } else {
      // add the self edge by updating the vertex weight
      graph->getVertexData(source)->self_weight = weight;
    }       
  }

  std::cout 
    << "Finished loading graph with: " << std::endl
    << "\t Vertices: " << graph->num_vertices() << std::endl
    << "\t Edges: " << graph->num_edges() << std::endl;
  graph->finalize();

//  std::cout << "Normalizing out edge weights." << std::endl;
  // This could be done in graphlab but the focus of this app is
  // demonstrating pagerank
  for(int vid = 0; 
      vid < graph->num_vertices(); ++vid) {
    vdata* vd = graph->getVertexData(vid);
    // Initialze with self out edge weight
    double sum = vd->self_weight;
    struct edge_info* out_edges = graph->getOutEdges(vid);

    // Sum up weight on out edges
    for(size_t i = 0; i < graph->getOutDegree(vid); ++i) {
      sum += graph->getEdgeData(out_edges[i].edge_id)->weight;
    }
    if (sum == 0) {
        vd->self_weight = 1.0;
        sum = 1.0; //Dangling page
    }

    assert(sum > 0);
    // divide everything by sum
    vd->self_weight /= sum;
    for(size_t i = 0; i < graph->getOutDegree(vid); ++i) {
      graph->getEdgeData(out_edges[i].edge_id)->weight /= sum;
    } 
  }
  //std::cout << "Finished normalizing edes." << std::endl;

  /*std::cout 
    << "Finalizing graph." << std::endl
    << "\t This is required for the locking protocol to function correctly"
    << std::endl;

  std::cout << "Finished finalization!" << std::endl;
*/
  return true;
} // end of load graph

bool terminated = false;
void pagerank_write_phase(int vid, void* scheduler_void) {
  terminated = false;
  vdata* vd = graph->getVertexData(vid);
  vd->value = vd->new_value;

  struct edge_info* in_edges = graph->getInEdges(vid);
  int in_degree = graph->getInDegree(vid);

  for (int i = 0; i < in_degree; i++) {
    // Get the edge data for the neighbor.
    edata* ed = graph->getEdgeData(in_edges[i].edge_id);
    ed->old_source_value = ed->new_source_value;
  }
}


/**
 * The Page rank update function
 */
void pagerank_update(int vid,
                     void* scheduler_void) {
  Scheduler* scheduler = (Scheduler*) scheduler_void;
  // Get the data associated with the vertex
  vdata* vd = graph->getVertexData(vid);
  
  // Sum the incoming weights; start by adding the 
  // contribution from a self-link.
  double sum = vd->value*vd->self_weight;

  struct edge_info* in_edges = graph->getInEdges(vid);
  int in_degree = graph->getInDegree(vid);

  for (int i = 0; i < in_degree; i++) {
    // Get the neighbor vertex value.
    vdata* neighbor_vdata = graph->getVertexData(in_edges[i].out_vertex);
    double neighbor_value = neighbor_vdata->value;

    // Get the edge data for the neighbor.
    edata* ed = graph->getEdgeData(in_edges[i].edge_id);
    
    // Compute the contribution for the neighbor, and add it to the sum.
    double contribution = ed->weight * neighbor_value;
    sum += contribution;

    // Remember this value as last read from the neighbor.
    ed->old_source_value = neighbor_value;
    //ed->new_source_value = neighbor_value;
  }

  // compute the jumpweight
  sum = random_reset_prob/graph->num_vertices() + 
    (1-random_reset_prob)*sum;
  vd->value = sum;
  //scheduler->add_task(vid, &pagerank_write_phase, 0);

  struct edge_info* out_edges = graph->getOutEdges(vid);
  int out_degree = graph->getOutDegree(vid);

  for (int i = 0; i < out_degree; i++) {
    edata* ed = graph->getEdgeData(out_edges[i].edge_id);
    // Compute edge-specific residual by comparing the new value of this
    // vertex to the previous value seen by the neighbor vertex.
    double residual = 
        ed->weight * std::fabs(ed->old_source_value - vd->value);

    // If the neighbor changed sufficiently add to scheduler.
    if (residual > termination_bound) {
      //if(terminated){
        terminated = false;
        //printf("terminated is false \n");
      //}
      //scheduler->add_task(out_edges[i].in_vertex, &pagerank_update, 1);
    }
  }
  scheduler->add_task(vid, &pagerank_update, 1);
} // end of pagerank update function

template<typename T>
T* Mat_to_Row_Major (Mat& in) {
    T* row_major = (T*) malloc(sizeof(T) * in.rows * in.cols);
    cilk_for (int row = 0; row < in.rows; row++) {
        Mat tmp2;
        in.row(row).convertTo(tmp2, CV_8U);
        T *ptemp = (T *) tmp2.ptr();
        for (int j = 0; j < in.cols; j++) {
          row_major[row*in.cols + j] = ptemp[j];
        }
    }
    return row_major;
}

Graph<vdata, edata>* row_major_to_graph (uint8_t* row_major, int rows, int cols, int seed_x, int seed_y) {
  Graph<vdata, edata>* graph = new Graph<vdata, edata>();
  graph->resize(rows*cols);
  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
          if (r+dx < 0 || r+dx >= rows) continue;
          if (c+dy < 0 || c+dy >= cols) continue;
          float weight = (float)1 / (float) abs(row_major[r*rows + c] - row_major[(r+dx)*rows + c + dy]);
          edata e(weight);
          int source = r*rows + c;
          int target = r*(rows+dx) + c + dy;
          if (source != target) {
            graph->addEdge(source, target, e);
          } else {
            graph->getVertexData(source)->self_weight = 0.5;
            graph->getVertexData(source)->value = 0;
            if (source == r*seed_y + seed_x) {
              graph->getVertexData(source)->value = 1;
            }
          }
        }
      }
    }
  }
  return graph;
}

int main(int argc, char **argv)
{
  Mat im;
  im = imread(argv[1],0);
  im.convertTo(im,CV_8U);
  printf("Success dims %d\n", im.dims);

  printf("Converting opencv mat to row major array\n");
  uint8_t* input_image = Mat_to_Row_Major<uint8_t>(im);

  int seed_x = 850;
  int seed_y = 900;

  Graph<vdata, edata>* graph = row_major_to_graph(input_image, im.rows, im.cols, seed_x, seed_y);


 printf("created graph with %d vertices and %d edges\n", graph->num_vertices(), graph->num_edges());
/*
  for (int i = 0; i < im.rows*im.cols; i++) {
    if (i < 1000*500) {
      input_image[i] = 100;
    }
    if (i < 1000*750  && i > 1000*750 + 750) {
      input_image[i] = 100;
    }
  }*/
  int colorCount = graph->compute_coloring_atomiccounter();
  printf("Graph colored using %d colors\n", colorCount);
  graph->validate_coloring();

  printf("About to run pagerank on image\n");
  scheduler = new Scheduler(graph->vertexColors, colorCount, graph->num_vertices(), 2);
  for (int i = 0; i < graph->num_vertices(); i++){
    scheduler->add_task(i, &pagerank_update, 1);
  }
  engine<vdata, edata>* e = new engine<vdata, edata>(graph, scheduler);

  double start = tfk_get_time();
  e->run(&terminated);
  double end = tfk_get_time();
  printf("done running\n");
  Mat tmp = Mat(im.rows, im.cols, CV_8U, input_image);
  imwrite("/afs/csail/u/t/tfk/public_html/test-image.jpg", tmp);
  return 0;
/*

  graph = new Graph<vdata, edata>();


  double load_start = tfk_get_time();
  load_graph_from_file(std::string(argv[1]));
  double load_end = tfk_get_time();

  bool full_output = false;
  if (argc > 2) {
    full_output = true;
    printf("full output \n");
  }

  double sum = 0;
  srand(1);
  for (int i = 0; i < graph->num_vertices(); i++) {
    graph->getVertexData(i)->value = 1 + tfkRand(0, 1);
    graph->getVertexData(i)->new_value = graph->getVertexData(i)->value;
    sum += graph->getVertexData(i)->value;
  }

  for (int i = 0; i < graph->num_vertices(); i++) {
    graph->getVertexData(i)->value = graph->getVertexData(i)->value / sum; 
  } 
  double color_start = tfk_get_time();
  int colorCount = graph->compute_coloring_atomiccounter();
  //int colorCount = graph->compute_trivial_coloring();
  //int colorCount = graph->compute_coloring();
  double color_end = tfk_get_time();
  printf("Time spent coloring %f \n", (color_end-color_start));
  printf("Number of colors %d \n", colorCount);
  graph->validate_coloring();
  scheduler = new Scheduler(graph->vertexColors, colorCount, graph->num_vertices(), 2);
  for (int i = 0; i < graph->num_vertices(); i++){ 
    scheduler->add_task(i, &pagerank_update, 1);
  }
  
  engine<vdata, edata>* e = new engine<vdata, edata>(graph, scheduler);
   

  double start = tfk_get_time();
  e->run(&terminated);
  double end = tfk_get_time();
  printf("\n Graph Coloring: %d colors used \n", colorCount);

  printf("\n*** Benchmark Results ***\n");
  printf("Time to load graph %g \n", load_end - load_start);
  printf("Time spent coloring %f \n", (color_end-color_start));
  printf("Time spent iterating %f \n", (end-start));
  printf("Total runtime (including loading graph) %f \n", load_end + color_end + end - load_start - color_start - start);
  printf("Total runtime (not including loading graph) %f \n", color_end + end - color_start - start);
  
  printf("\n*** First 5 pagerank values ***\n");
  int output_num = 5;
  if (full_output) {
    output_num = graph->num_vertices();
  }

  double norm = 0;
  // compute a normalizer.
  for (int i = 0; i < graph->num_vertices(); i++) {
    norm += graph->getVertexData(i)->value;
  }
  printf ("the norm is %f \n", norm);
  for (int i = 0; i < output_num; i++) {
    printf("vertex %d value is %g \n", i, graph->getVertexData(i)->value/norm);
  }
  return 0;*/
}

