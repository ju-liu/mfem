// Example: generate Klein bottle meshes
//
// Compile with: make klein-bottle
//
// Sample runs:  klein-bottle
//               klein-bottle -o 6 -nx 6 -ny 4

#include "mfem.hpp"
#include <fstream>
#include <iostream>

using namespace std;
using namespace mfem;

void figure8_trans(const Vector &x, Vector &p);

int main(int argc, char *argv[])
{
   const char *new_mesh_file = "klein-bottle.mesh";
   int nx = 16;
   int ny = 8;
   int order = 3;
   bool dg_mesh = false;
   bool visualization = true;

   OptionsParser args(argc, argv);
   args.AddOption(&new_mesh_file, "-m", "--mesh-out-file",
                  "Output Mesh file to write.");
   args.AddOption(&nx, "-nx", "--num-elements-x",
                  "Number of elements in x-direction.");
   args.AddOption(&ny, "-ny", "--num-elements-y",
                  "Number of elements in y-direction.");
   args.AddOption(&order, "-o", "--mesh-order",
                  "Order (polynomial degree) of the mesh elements.");
   args.AddOption(&dg_mesh, "-dm", "--discont-mesh", "-cm", "--cont-mesh",
                  "Use dicontinuous or continuous space for the mesh nodes.");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.Parse();
   if (!args.Good())
   {
      args.PrintUsage(cout);
      return 1;
   }
   args.PrintOptions(cout);

   Mesh *mesh;
   Element::Type el_type = Element::QUADRILATERAL;
   // Element::Type el_type = Element::TRIANGLE;
   mesh = new Mesh(nx, ny, el_type, 1, 2*M_PI, 2*M_PI);

   mesh->SetCurvature(order, true, 3, Ordering::byVDIM);

   {
      Array<int> v2v(mesh->GetNV());
      for (int i = 0; i < v2v.Size(); i++)
      {
         v2v[i] = i;
      }
      for (int i = 0; i <= nx; i++)
      {
         int v_old = i + ny * (nx + 1);
         int v_new = i;
         v2v[v_old] = v_new;
      }
      for (int j = 0; j <= ny; j++)
      {
         int v_old = nx + j * (nx + 1);
         int v_new = (ny - j) * (nx + 1);
         v2v[v_old] = v2v[v_new];
      }
      for (int i = 0; i < mesh->GetNE(); i++)
      {
         Element *el = mesh->GetElement(i);
         int *v = el->GetVertices();
         int nv = el->GetNVertices();
         for (int j = 0; j < nv; j++)
         {
            v[j] = v2v[v[j]];
         }
      }
      for (int i = 0; i < mesh->GetNBE(); i++)
      {
         Element *el = mesh->GetBdrElement(i);
         int *v = el->GetVertices();
         int nv = el->GetNVertices();
         for (int j = 0; j < nv; j++)
         {
            v[j] = v2v[v[j]];
         }
      }
      mesh->RemoveUnusedVertices();
   }

   mesh->Transform(figure8_trans);

   if (!dg_mesh)
   {
      mesh->SetCurvature(order, false, 3, Ordering::byVDIM);
   }

   GridFunction &nodes = *mesh->GetNodes();
   for (int i = 0; i < nodes.Size(); i++)
   {
      if (std::abs(nodes(i)) < 1e-12)
      {
         nodes(i) = 0.0;
      }
   }

   ofstream ofs(new_mesh_file);
   ofs.precision(8);
   mesh->Print(ofs);
   ofs.close();

   if (visualization)
   {
      char vishost[] = "localhost";
      int  visport   = 19916;
      socketstream sol_sock(vishost, visport);
      sol_sock.precision(8);
      sol_sock << "mesh\n" << *mesh << flush;
   }

   delete mesh;
}

void figure8_trans(const Vector &x, Vector &p)
{
   const double r = 2.5;
   double a = r + cos(x(0)/2) * sin(x(1)) - sin(x(0)/2) * sin(2*x(1));

   p.SetSize(3);
   p(0) = a * cos(x(0));
   p(1) = a * sin(x(0));
   p(2) = sin(x(0)/2) * sin(x(1)) + cos(x(0)/2) * sin(2*x(1));
}
