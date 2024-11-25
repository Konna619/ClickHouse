import { valueToColor, determineTickStep } from './visualizeHelpers.js';
import { infoButton } from './infoButton.js';

function formatNumber(number)
{
    if (number >= Math.pow(1000, 4))
        return (number / Math.pow(1000, 4)).toFixed(1) + 'T';
    else if (number >= Math.pow(1000, 3))
        return (number / Math.pow(1000, 3)).toFixed(1) + 'G';
    else if (number >= Math.pow(1000, 2))
        return (number / Math.pow(1000, 2)).toFixed(1) + 'M';
    else if (number >= 1000)
        return (number / 1000).toFixed(1) + 'K';
    else
        return number;
}

function formatBytes(bytes)
{
    const digits = 0;
    if (bytes >= Math.pow(1024, 4))
        return (bytes / Math.pow(1024, 4)).toFixed(digits) + 'TB';
    else if (bytes >= Math.pow(1024, 3))
        return (bytes / Math.pow(1024, 3)).toFixed(digits) + 'GB';
    else if (bytes >= Math.pow(1024, 2))
        return (bytes / Math.pow(1024, 2)).toFixed(digits) + 'MB';
    else if (bytes >= 1024)
        return (bytes / 1024).toFixed(digits) + 'KB';
    else
        return bytes;
}

class MergeTreeVisualizer {
    getMargin() { return { left: 60, right: 30, top: 60, bottom: 60 }; }
    getXAxisTitleOffset() { return { x: 0, y: 0 }; }
    getYAxisTitleOffset() { return { x: 0, y: 0 }; }

    getLeft(part) { return part.left_bytes; }
    getRight(part) { return part.right_bytes; }

    // Override these to adjust values only for drawing merges
    getMergeLeft(part) { return this.xScale(this.getLeft(part)); }
    getMergeRight(part) { return this.xScale(this.getRight(part)); }
    getMergeTop(part) { return this.yScale(this.getTop(part)); }
    getMergeBottom(part) { return this.yScale(this.getBottom(part)); }

    // Override these to adjust values only for drawing parts
    getPartLeft(part) { return this.xScale(this.getLeft(part)); }
    getPartRight(part) { return this.xScale(this.getRight(part)); }
    getPartTop(part) { return this.yScale(this.getBottom(part)) + (this.isYAxisReversed() ? 0 : -this.part_height); }
    getPartBottom(part) { return this.yScale(this.getBottom(part)) + (this.isYAxisReversed() ? this.part_height : 0); }

    // Colors
    getMergeColor() { return "red"; }
    getPartColor(part) { return part.merging ? "orange" : "black"; }
    getPartMarkColor(part) { return "yellow"; }

    isYAxisReversed() { return false; }

    constructor(mt, container) {
        // Cleanup previous visualization
        const oldSvg = container.select("svg");
        if (oldSvg.node()) {
            if (oldSvg.node().__tippy)
                oldSvg.node().__tippy.destroy();
        }
        oldSvg.remove();

        this.xAxisTitleOffset = this.getXAxisTitleOffset();
        this.yAxisTitleOffset = this.getYAxisTitleOffset();

        // Input visuals (common settings)
        this.margin = this.getMargin();
        this.width = 800;
        this.height = 350;
        this.part_height = 4;
        this.part_mark_width = 1;
        this.svgWidth = this.width + this.margin.left + this.margin.right;
        this.svgHeight = this.height;

        // Compute useful aggregates
        this.log_min_bytes = Math.log2(d3.min(mt.parts, d => d.bytes));
        this.log_max_bytes = Math.log2(d3.max(mt.parts, d => d.bytes));
        this.max_source_part_count = d3.max(mt.parts, d => d.source_part_count);

        // Compute scale ranges
        this.computeXAggregates(mt);
        this.computeYAggregates(mt);

        // Create the SVG container
        this.svgContainer = container
            .append("svg")
            .attr("width", this.svgWidth)
            .attr("height", this.svgHeight);
    }

    computeXAggregates(mt) {
        this.minXValue = d3.min(mt.parts, d => this.getLeft(d));
        this.maxXValue = d3.max(mt.parts, d => this.getRight(d));
    }

    computeYAggregates(mt) {
        if (this.isYAxisReversed()) {
            this.minYValue = d3.min(mt.parts, d => this.getTop(d));
            this.maxYValue = d3.max(mt.parts, d => this.getBottom(d));
        } else {
            this.minYValue = d3.min(mt.parts, d => this.getBottom(d));
            this.maxYValue = d3.max(mt.parts, d => this.getTop(d));
        }
    }

    initXScaleLinear() {
        // Set up the horizontal scale (x-axis) — linear scale
        this.xScale = d3.scaleLinear()
            .range([this.margin.left, this.svgWidth - this.margin.right]);

        this.updateXDomain = () => this.xScale.domain([this.minXValue, this.maxXValue]);
        this.updateXDomain();
    }

    getYRange() {
        const range = [this.svgHeight - this.margin.bottom, this.margin.top];
        return this.isYAxisReversed() ? [range[1], range[0]] : range;
    }

    initYScalePowersOfTwo() {
        // Set up the vertical scale (y-axis) — logarithmic scale
        this.yScale = d3.scaleLog()
            .base(2)
            .range(this.getYRange());

        this.updateYDomain = () => this.yScale.domain([Math.max(1, this.minYValue), Math.pow(2, Math.ceil(Math.log2(this.maxYValue)))])
        this.updateYDomain();
    }

    initYScaleLinear() {
        // Set up the vertical scale (y-axis) — linear scale
        this.yScale = d3.scaleLinear()
            .range(this.getYRange());

        this.updateYDomain = () => this.yScale.domain([this.minYValue, this.maxYValue])
        this.updateYDomain();
    }

    createXAxisLinear() {
        this.xAxisFormatter = formatBytes;
        this.xAxisTickValues = () => d3.range(this.minXValue, this.maxXValue, determineTickStep(this.maxXValue - this.minXValue, 1024));
        this.xAxisType = this.isYAxisReversed() ? d3.axisTop : d3.axisBottom;

        const translateY = this.isYAxisReversed() ? this.margin.top : this.svgHeight - this.margin.bottom;
        this.xAxis = this.xAxisType(this.xScale)
            .tickValues(this.xAxisTickValues())
            .tickFormat(this.xAxisFormatter);
        this.xAxisGroup = this.svgContainer.append("g")
            .attr("transform", `translate(0, ${translateY})`)
            .call(this.xAxis);

        // Add axis title
        this.svgContainer.append("text")
            .attr("x", this.svgWidth / 2 + this.xAxisTitleOffset.x)
            .attr("y", this.isYAxisReversed() ? (this.margin.top / 2 + this.xAxisTitleOffset.y) : (this.svgHeight - this.margin.bottom / 2 + this.xAxisTitleOffset.y) )
            .attr("text-anchor", "middle")
            .attr("font-size", "14px")
            .text("Bytes");
    }

    createYAxisPowersOfTwo() {
        const powersOfTwo = Array.from({ length: 50 }, (v, i) => Math.pow(2, i + 1));

        this.yAxisFormatter = formatBytes; // d => `2^${Math.log2(d)}`;
        this.yAxisTickValues = () => powersOfTwo.filter(d => d >= this.minYValue && d <= this.maxYValue)
        this.yAxisType = d3.axisLeft;

        this.yAxis = this.yAxisType(this.yScale)
            .tickValues(this.yAxisTickValues())
            .tickFormat(this.yAxisFormatter);
        this.yAxisGroup = this.svgContainer.append("g")
            .attr("transform", `translate(${this.margin.left}, 0)`)
            .call(this.yAxis);

        // Add axis title
        this.svgContainer.append("text")
            .attr("transform", "rotate(-90)")
            .attr("x", -(this.svgHeight / 2 + this.yAxisTitleOffset.y))
            .attr("y", this.margin.left / 2 + this.yAxisTitleOffset.x)
            .attr("text-anchor", "middle")
            .attr("font-size", "14px")
            .text("Log(PartSize)");

        // This does not work with updating, switched to formatBytes
        // this.yAxisGroup.selectAll(".tick text")
        //     .each(function(d) {
        //         const exponent = Math.log2(d);
        //         const self = d3.select(this);
        //         self.text("");
        //         self.append("tspan").text("2");
        //         self.append("tspan")
        //             .attr("dy", "-0.7em")
        //             .attr("font-size", "70%").text(exponent);
        //     });
    }

    createYAxisLinear() {
        this.yAxisFormatter = formatNumber;
        this.yAxisTickValues = () => d3.range(this.minYValue, this.maxYValue + 1, determineTickStep(this.maxYValue - this.minYValue, 1000));
        this.yAxisType = d3.axisLeft;
        this.yAxis = this.yAxisType(this.yScale)
            .tickValues(this.xAxisTickValues())
            .tickFormat(this.yAxisFormatter);
        this.yAxisGroup = this.svgContainer.append("g")
            .attr("transform", `translate(${this.margin.left}, 0)`)
            .call(this.yAxis);

        // Add axis title
        this.svgContainer.append("text")
            .attr("transform", "rotate(-90)")
            .attr("x", -(this.svgHeight / 2 + this.yAxisTitleOffset.y))
            .attr("y", this.margin.left / 2 + this.yAxisTitleOffset.x)
            .attr("text-anchor", "middle")
            .attr("font-size", "14px")
            .text("Source parts count");
    }

    updateX(mt) {
        // Rescale axis
        this.computeXAggregates(mt);
        this.updateXDomain();

        // Update axes with transitions
        this.xAxis = this.xAxisType(this.xScale)
        this.xAxisGroup //.transition().duration(100)
            .call(
                this.xAxisType(this.xScale)
                    .tickFormat(this.xAxisFormatter)
                    .tickValues(this.xAxisTickValues())
            );
    };

    updateY(mt) {
        // Rescale axis
        this.computeYAggregates(mt);
        this.updateYDomain();

        // Update axes with transitions
        this.yAxisGroup //.transition().duration(100)
            .call(
                this.yAxisType(this.yScale)
                    .tickFormat(this.yAxisFormatter)
                    .tickValues(this.yAxisTickValues())
            );
    };

    createDescription(text, x = 10, y = 60) {
        this.svgContainer.node().__tippy = infoButton(this.svgContainer, x, y, text);
    }

    pxl(value, min = 0) { return Math.max(min, Math.floor(value)); }
    pxr(value, min = 1) { return Math.max(min, Math.ceil(value)); }
    pxt(value) { return value; }
    pxb(value) { return Math.max(1, value); }

    processMerges(mt) {
        // Check if the merges group already exists, create it if not, and save the reference
        if (!this.mergesGroup) {
            this.mergesGroup = this.svgContainer.append("g").attr("class", "viz-merge");
        }

        // Filter inactive parts and join the data to the rectangles
        const inactiveParts = mt.parts.filter(d => !d.active);
        const merges = this.mergesGroup.selectAll("rect")
            .data(inactiveParts, d => d.id); // Assuming each part has a unique 'id'

        // Handle the enter phase for new elements
        const mergesEnter = merges.enter().append("rect");

        // Merge the enter and update selections, and set attributes for both
        mergesEnter.merge(merges)
            .attr("x", d => this.pxl(this.getMergeLeft(d)))
            .attr("y", d => this.pxt(this.getMergeTop(d)))
            .attr("width", d => this.pxr(this.getMergeRight(d) - this.getMergeLeft(d)))
            .attr("height", d => this.pxb(this.getMergeBottom(d) - this.getMergeTop(d)))
            .attr("fill", d => this.getMergeColor(d));

        // Handle the exit phase for removed elements
        merges.exit().remove();
    }

    processParts(mt) {
        // Check if the parts group already exists, create it if not, and save the reference
        if (!this.partsGroup) {
            this.partsGroup = this.svgContainer.append("g").attr("class", "viz-part");
        }

        // Join the data to the rectangles
        const parts = this.partsGroup.selectAll("rect")
            .data(mt.parts, d => d.id); // Assuming each part has a unique 'id'

        // Handle the enter phase for new elements
        const partsEnter = parts.enter()
            .append("rect");

        // Merge the enter and update selections, and set attributes for both
        partsEnter.merge(parts)
            .attr("x", d => this.pxl(this.getPartLeft(d)))
            .attr("y", d => this.pxt(this.getPartTop(d)))
            .attr("width", d => this.pxr(this.getPartRight(d) - this.getPartLeft(d)))
            .attr("height", d => this.pxb(this.getPartBottom(d) - this.getPartTop(d)))
            .attr("fill", d => this.getPartColor(d));

        // Handle the exit phase for removed elements
        parts.exit().remove();
    }

    processPartMarks(mt) {
        // Check if the part marks group already exists, create it if not, and save the reference
        if (!this.partMarksGroup) {
            this.partMarksGroup = this.svgContainer.append("g").attr("class", "viz-part-mark");
        }

        // Join the data to the rectangles
        const partMarks = this.partMarksGroup.selectAll("rect")
            .data(mt.parts, d => d.id); // Assuming each part has a unique 'id'

        // Handle the enter phase for new elements
        const partMarksEnter = partMarks.enter().append("rect");

        // Merge the enter and update selections, and set attributes for both
        partMarksEnter.merge(partMarks)
            .attr("x", d => this.pxl(this.getPartLeft(d)))
            .attr("y", d => this.pxt(this.getPartTop(d)))
            .attr("width", d => this.pxr(Math.min(this.part_mark_width, this.getPartRight(d) - this.getPartLeft(d))))
            .attr("height", d => this.pxb(this.getPartBottom(d) - this.getPartTop(d)))
            .attr("fill", d => this.getPartMarkColor(d));

        // Handle the exit phase for removed elements
        partMarks.exit().remove();
    }

    update(mt) {
        this.updateX(mt);
        this.updateY(mt);

        this.processMerges(mt);
        this.processParts(mt);
        this.processPartMarks(mt);
    }
}

class MergeTreeUtilityVisualizer extends MergeTreeVisualizer {
    getMargin() { return { left: 60, right: 30, top: 60, bottom: 60 }; }
    getXAxisTitleOffset() { return { x: 0, y: 5 }; }
    getYAxisTitleOffset() { return { x: -17, y: 0 }; }

    getTop(part) { return part.active ? part.bytes : part.parent_part.bytes; }
    getBottom(part) { return part.bytes; }

    getMergeColor(part) {
        // It is a constant to get consistent colors on all diagrams, merging more than 128 parts is not common
        const max_entropy = 7;
        return valueToColor(part.parent_part.entropy, 0, max_entropy);
    }

    constructor(mt, container) {
        super(mt, container);

        this.initXScaleLinear();
        this.initYScalePowersOfTwo();

        this.processMerges(mt);
        this.processParts(mt);
        this.processPartMarks(mt);

        this.createXAxisLinear();
        this.createYAxisPowersOfTwo();

        this.createDescription(`
            <h5 align="center">Utility diagram</h5>

            <h6>Parts</h6>
            <p>
            The diagram is constructed from the bottom upward.
            Initial parts are at the bottom.
            Part is a horizontal black bar with a yellow mark on the left side.
            Bar width equals part size.
            Parts are positioned on the X-axis in the insertion order: older part are on the left, newer are on the right.
            The y-position of a part equals the logarithm of its size.
            </p>

            <h6>Merges</h6>
            <p>
            One merge is represented by shape that consists of adjacent rectangles of the same color.
            One rectangle per every child (source part).
            Every rectangle connects the child at the bottom to the parent (resulting) part at the top.
            The area of a merge represents its <u>utility</u>.
            Color represents average height of merge, <u>entropy</u>: its area divided by its width.
            </p>

            <h6>Utility</h6>
            <p>
            Note that total diagram area does not depend on merge tree structure: it is a function of initial and final part sizes.
            So larger utility of a merge means "more" progress toward the "all parts are merged" state.
            The utility is a measure that shows the overall "progress" of the merging process,
            a total "distance" that all byte of data should travel upwards to be merged:

            $$U = B \\log B - \\sum_{i} b_{i} \\log b_{i}$$

            where $B$ – size of result, $b_{i}$ – size of $i$-th child.
            </p>

            <h6>Entropy</h6>
            <p>
            Consider a vertical line on the diagram.
            It show a path of one single byte from the initial bottom part to the final top part.
            Number of times this line intersects black bars is equal to the write amplification.
            To lower write amplification it is important to have larger distance between bars.
            Entropy of a merge is equal to average (per byte) distance between bars:

            $$H = \\frac{U}{B} = - \\sum_{i} p_{i} \\log p_{i}$$
            where $p_{i}$ – probability to randomly select a byte from $i$-th child.
            </p>
        `);
    }
}

class MergeTreeTimeVisualizer extends MergeTreeVisualizer {
    getMargin() { return { left: 60, right: 30, top: 60, bottom: 60 }; }
    getXAxisTitleOffset() { return { x: 0, y: 0 }; }
    getYAxisTitleOffset() { return { x: -7, y: 0 }; }

    isYAxisReversed() { return true; }
    getTop(part) { return part.active ? 0 : this.getBottom(part.parent_part); }
    getBottom(part) { return part.active ? 0 : part.parent_part.source_part_count + this.getBottom(part.parent_part); }

    getMergeTop(part) {  return this.yScale(this.getTop(part)); }
    getMergeBottom(part) { return this.yScale(this.getBottom(part)); }

    getMergeColor(part) {
        // TODO: choose consistent color scheme.
        // TODO: There is no point in showing height again with color
        return valueToColor(part.parent_part.source_part_count, 2, this.max_source_part_count, 90, 70, 270, 180);
    }

    constructor(mt, container) {
        super(mt, container);

        this.initXScaleLinear();
        this.initYScaleLinear();

        this.processMerges(mt);
        this.processParts(mt);
        this.processPartMarks(mt);

        this.createXAxisLinear();
        this.createYAxisLinear();

        this.createDescription(`
            <h5 align="center">Time diagram</h5>

            <h6>Parts</h6>
            <p>
            The diagram is constructed from the top downwards.
            Final parts are at the top.
            Part is a horizontal yellow bar with a black mark on the left side.
            Bar width equals part size.
            Parts are positioned on the X-axis in the insertion order: older part are on the left, newer are on the right.
            The y-position of a part depends of history of merges (see below).
            </p>

            <h6>Merges</h6>
            <p>
            One merge is represented by one rectangle.
            It connects the children at the bottom to the parent (resulting) part at the top.
            The height of the rectangle equals the number of source parts.
            It determines the y-position of all parts relative to their parents.
            Color also represents the number of source parts in the merge.
            </p>

            <h6>Execution time</h6>
            <p>
            The X-axis is in bytes.
            In model merge duration is proportional to number of bytes to be written.
            So width of merges also represents duration (execution time) of a merge.
            Such a model assumes there is no overhead for merges and speed of all merges expressed in bytes written per second is the same.
            </p>

            <h6>Part count time integral</h6>
            <p>
            Average active part count is computed as an integral of part count over time interval:
            $$I = T \\cdot \\mathbb{E}[Active] = \\int_0^T Active(t) \\, dt$$
            To minimize part count one should minimize the integral.
            It can be computed as total area of all rectangles in the diagram plus "waiting" time $W$.
            The area of one merge rectangle equals its contribution to the integral.
            $$I = W + \\sum_{i} d_{i} \\cdot n_{i}$$
            where $d_{i}$ – merge execution time of $i$-th part, $n_{i}$ – number of children of $i$-th part.
            </p>
            <p>
            It is important to note that diagram does not show "waiting" time, while parts are active, but not merging.
            </p>
        `, 15, 60);
    }
}

export { MergeTreeUtilityVisualizer, MergeTreeTimeVisualizer };
