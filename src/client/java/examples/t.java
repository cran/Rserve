import org.rosuda.JRclient.*;
import java.io.*;
import java.util.*;

// this is just a collection of various silly things you can do when
// accessing Rserve. it's not very useful in particular, but it may
// contain some demo code

public class t {
    public static void main(String args[]) {
	try {
	    Rconnection c=new Rconnection((args.length>0)?args[0]:"127.0.0.1");

	    System.out.println("ok, connected, press <enter> to continue\n");
	    System.in.read();

	    /*	    {
		REXP x=c.eval("c(\"bla\",\"blu\",\"ble\")");
		System.out.println("x="+x);
		Vector v=(Vector)x.getContent();
		System.out.println("v.size()="+v.size());
		
		int i=0;
		while (i<v.size()) {
		    System.out.println("v("+i+")="+((REXP)v.elementAt(i)).asString());
		    i++;
		}
		System.exit(0);
		} */

	    // lowess example
	    System.out.println("lowess: create points");
	    double[] dataX = (double[]) c.eval("rnorm(100)").getContent();
	    double[] dataY = (double[]) c.eval("rnorm(100)").getContent();
	    System.out.println("lowess: assign points");
	    c.assign("x",dataX);
	    c.assign("y",dataY);
	    System.out.println("lowess: call lowess");
	    RList l = c.eval("lowess(x,y)").asList();
	    System.out.println("lowess: retrieve points");
	    double[] lx = (double[]) l.at("x").getContent();
	    double[] ly = (double[]) l.at("y").getContent();

	    // matrix test
	    System.out.println("matrix: create matrix");
	    int m=100, n=100;
	    double[] mat=new double[m*n];
	    int i=0;
	    while (i<m*n) mat[i++]=i/100;
	    System.out.println("matrix: assign matrix");
	    c.assign("m",mat);
	    c.voidEval("m<-matrix(m,"+m+","+n+")");
	    System.out.println("matrix: cross-product");
	    double[][] mr=c.eval("crossprod(m,m)").asDoubleMatrix();

	    // I/O test
	    System.out.println("I/O test (this will fail if I/O is disabled)");
	    // create a file on the R side
	    RFileOutputStream os=c.createFile("test.txt");
	    PrintStream ps=new PrintStream(os);
	    ps.println("A\tB");
	    ps.println("1\t4");
	    ps.println("4\t6");
	    ps.close();

	    // let's read that file as a data set to prove that it's on the server
	    c.voidEval("d<-read.table(\"test.txt\",TRUE)");
	    double r=c.eval("sum(d$A)/sum(d$B)").asDouble();
	    System.out.println("sum(A)/sum(B)="+r);

	    // let's read the file back - just to see how to read files
	    RFileInputStream is=c.openFile("test.txt");
	    byte[] buf=new byte[1024];
	    System.out.println("read "+is.read(buf)+" bytes.");
	    System.out.println(new String(buf));
	    is.close();

	    // ok, we're done, so remove the file
	    // if you fail to remove the file, the entire working directory
	    // will be retained.
	    c.removeFile("test.txt");

	    // close Rconnection, we're done
	    c.close();
        } catch(RSrvException rse) {
            System.out.println("Rserve exception: "+rse.getMessage());
        } catch(Exception e) {
            System.out.println("Something went wrong, but it's not the Rserve: "
+e.getMessage());
            e.printStackTrace();
        }
    }
}
