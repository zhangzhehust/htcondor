//******************************************************************************
// attrlist.h
//
// Definition of AttrList classes and AttrListList class.
//
//******************************************************************************

#ifndef _ATTRLIST_H
#define _ATTRLIST_H

#include <stdio.h>

#include "condor_expressions.h"
#include "condor_exprtype.h"
#include "condor_astbase.h"

//for the shipping functions
#if defined(USE_XDR)
#include <rpc/types.h>
#include <rpc/xdr.h>
#endif
#include "stream.h"

#define		ATTRLIST_MAX_EXPRESSION		1024

enum							// various AttrLists
{
    ATTRLISTENTITY,
    ATTRLISTREP
};
enum {AGG_INSERT, AGG_REMOVE};	// operations on aggregate classes

class AttrListElem
{
    public :

        AttrListElem(ExprTree*);			// constructor
        AttrListElem(AttrListElem&);		// copy constructor
        ~AttrListElem() { delete tree; }	// destructor

        friend class AttrList;
        friend class ClassAd;
        friend class AttrListList;
  
    private :

        ExprTree*		tree;	// the tree pointed to by this element
		int				dirty;	// has this tree been changed?
		char*			name;	// the name of the tree
        AttrListElem*	next;	// next element in the list
};

class AttrListAbstract
{
    public :

		int		Type() { return type; }		// type of the AttrList

		friend	class		AttrList;
		friend	class		AttrListList;
		friend	class		ClassAd;
		friend	class		ClassAdList;

    protected :

		AttrListAbstract(int);
		virtual ~AttrListAbstract() {}

		int					type;		// type of this AttrList
		class AttrListList*	inList;		// I'm in this AttrList list
		AttrListAbstract*	next;		// next AttrList in the list
		AttrListAbstract*	prev;		// previous AttrList in the list
};

class AttrListRep: public AttrListAbstract
{
    public:

        AttrListRep(AttrList*, AttrListList*);	// constructor

		friend			AttrList;
		friend			AttrListList;

    private:

        AttrList*		attrList;		// the original AttrList 
        AttrListRep*	nextRep;		// next copy of original AttrList 
};

class AttrList : public AttrListAbstract
{
    public :

		// ctors/dtor
		AttrList();							// No associated AttrList list
        AttrList(AttrListList*);			// Associated with AttrList list
        AttrList(FILE *, char *, int &);	// Constructor, read from file.
//		AttrList(class ProcObj*);			// create from a proc object
		AttrList(CONTEXT*);					// create from a CONTEXT
        AttrList(char *, char);				// Constructor, from string.
        AttrList(AttrList&);				// copy constructor
        virtual ~AttrList();				// destructor

		// insert expressions into the ad
        int        	Insert(char*);			// insert at the tail
        int        	Insert(ExprTree*);		// insert at the tail
		int			InsertOrUpdate(char*);

		// deletion of expressions	
        int			Delete(const char*); 	// delete the expr with the name

		// to update expression trees
		void		ResetFlags();			// reset the all the dirty flags
		int			UpdateExpr(char*, ExprTree*);	// update an expression
		int			UpdateExpr(ExprTree*);

		// for iteration through expressions
		void		ResetExpr() { this->ptrExpr = exprList; }
		ExprTree*	NextExpr();					// next unvisited expression

		// for iteration through names (i.e., lhs of the expressions)
		void		ResetName() { this->ptrName = exprList; }
		char*		NextName();					// next unvisited name

		// lookup values in classads  (for simple assignments)
		ExprTree*   Lookup(char *);  		// for convenience
        ExprTree*	Lookup(const char*);	// look up an expression
		ExprTree*	Lookup(const ExprTree*);
		int         LookupString(const char *, char *); 
        int         LookupInteger(const char *, int &);
        int         LookupFloat(const char *, float &);
        int         LookupBool(const char *, int &);

		// evaluate values in classads
		int         EvalString (const char *, AttrList *, char *);
		int         EvalInteger (const char *, AttrList *, int &);
		int         EvalFloat (const char *, AttrList *, float &);
		int         EvalBool  (const char *, AttrList *, int &);

		int			IsInList(AttrListList*);	// am I in this AttrList list?

		// output functions
		int			fPrintExpr(FILE*, char*);	// print an expression
        virtual int	fPrint(FILE*);				// print the AttrList to a file

		// conversion function
		int         MakeContext (CONTEXT *);    // create a context

        // shipping functions
        int put(Stream& s);
        int get(Stream& s);
        int code(Stream& s);
#if defined(USE_XDR)
		int put (XDR *);
		int get (XDR *);
#endif

		friend	class	AttrListRep;			// access "next" 
		friend	class	AttrListList;			// access "UpdateAgg()"
		friend	class	ClassAd;

    protected :

		// update an aggregate expression if the AttrList list associated with
		// this AttrList is changed
      	int				UpdateAgg(ExprTree*, int);
		// convert a (key, value) pair to an assignment tree. used by the
		// constructor that builds an AttrList from a proc structure.
		ExprTree*		ProcToTree(char*, LexemeType, int, float, char*);
        AttrListElem*	exprList;		// my collection of expressions
		AttrListList*	associatedList;	// the AttrList list I'm associated with
		AttrListElem*	tail;			// used by Insert
		AttrListElem*	ptrExpr;		// used by NextExpr 
		AttrListElem*	ptrName;		// used by NextName
		int				seq;			// sequence number
};

class AttrListList
{
    public:
    
        AttrListList();					// constructor
        AttrListList(AttrListList&);	// copy constructor
        virtual ~AttrListList();		// destructor

        void 	  	Open();				// set pointer to the head of the queue
        void 	  	Close();			// set pointer to NULL
        AttrList* 	Next();				// return AttrList pointed to by "ptr"
        ExprTree* 	Lookup(const char*, AttrList*&);	// look up an expression
      	ExprTree* 	Lookup(const char*);

      	void 	  	Insert(AttrList*);	// insert at the tail of the list
      	int			Delete(AttrList*); 	// delete a AttrList

      	void  	  	fPrintAttrListList(FILE *); 	// print out the list
      	int 	  	MyLength() { return length; } 	// length of this list
      	ExprTree* 	BuildAgg(char*, LexemeType);	// build aggregate expr

      	friend	  	class		AttrList;
      	friend	  	class		ClassAd;
  
    protected:

        // update aggregate expressions in associated AttrLists
		void				UpdateAgg(ExprTree*, int);

        AttrListAbstract*	head;			// head of the list
        AttrListAbstract*	tail;			// tail of the list
        AttrListAbstract*	ptr;			// used by NextAttrList
        AttrListList*		associatedAttrLists;	// associated AttrLists
        int					length;			// length of the list
};

#endif
